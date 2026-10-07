#!/usr/bin/env python3
"""Assemble an allowlisted APK feed. Never copy a build directory wholesale."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK_SHA256 = "28e004c1be4d215d19c1f12a6aa4c8d8f80689549eb707d0ff5a71f16fa8d05f"
PACKAGES = {
    "agent-netd": "agent-netd", "agentd": "agentd", "agent-gw": "agent-gw",
    "agent-adapter": "agent-adapter", "nexus-cloud-connector": "nexus-cloud-connector",
    "luci-app-agent-router": "luci-app-agent-router",
    "nexus-agent-roles": "nexus-agent-services", "nexus-agent-relayd": "nexus-agent-services",
    "nexus-agent-directoryd": "nexus-agent-services", "nexus-node-runtime": "nexus-node-runtime",
    "nexus-agent-router": "nexus-agent-profiles", "nexus-agent-router-relay": "nexus-agent-profiles",
    "nexus-agent-router-seed": "nexus-agent-profiles",
}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def recipe_field(recipe, field):
    matches = re.findall(r"^" + re.escape(field) + r":=([^\r\n]+)$", recipe, re.M)
    if len(matches) != 1:
        raise ValueError(f"Expected one {field}")
    value = matches[0].strip()
    if not re.fullmatch(r"[A-Za-z0-9_.+:-]+", value):
        raise ValueError(f"Unsafe {field}")
    return value


def inventory(source):
    result = {}
    for name, recipe_dir in PACKAGES.items():
        recipe = (source / "feed" / recipe_dir / "Makefile").read_text(encoding="utf-8")
        version = recipe_field(recipe, "PKG_VERSION") + "-r" + recipe_field(recipe, "PKG_RELEASE")
        result[name] = {"version": version, "file": f"{name}-{version}.apk"}
    return result


def run(*args, **kwargs):
    return subprocess.check_output([str(arg) for arg in args], **kwargs)


def assemble(args):
    if not re.fullmatch(r"v[0-9][A-Za-z0-9.-]*", args.version):
        raise ValueError("Invalid release version")
    if digest(args.sdk_archive) != SDK_SHA256:
        raise ValueError("SDK archive does not match the pinned 25.12.4 x86/64 SDK")
    if args.output.exists():
        raise ValueError("Output must be a new directory")
    config = args.build_config.read_text(encoding="utf-8")
    if "CONFIG_TARGET_x86_64=y" not in config.splitlines():
        raise ValueError("Expected x86/64 build configuration")
    if args.private_key.stat().st_mode & 0o077:
        raise ValueError("Signing key must be private (chmod 600)")
    expected = run("openssl", "pkey", "-in", args.private_key, "-pubout", "-outform", "DER")
    actual = run("openssl", "pkey", "-pubin", "-in", args.public_key, "-outform", "DER")
    if expected != actual:
        raise ValueError("Signing key and public key do not match")
    commit = run("git", "-C", ROOT, "rev-parse", "HEAD").decode().strip()
    if run("git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no").strip():
        raise ValueError("Commit release sources before packaging")
    packages = inventory(ROOT)
    feed = args.sdk / "bin/packages/x86_64/nexus_agent_router"
    for item in packages.values():
        path = feed / item["file"]
        if not path.is_file() or path.is_symlink():
            raise ValueError("Missing regular package: " + item["file"])
        item["sha256"] = digest(path)
        item["bytes"] = path.stat().st_size
    node_recipe = (ROOT / "feed/nexus-node-runtime/Makefile").read_text(encoding="utf-8")
    node_version = recipe_field(node_recipe, "PKG_VERSION")
    node_source = args.sdk / "dl" / f"node-v{node_version}.tar.gz"
    if digest(node_source) != recipe_field(node_recipe, "PKG_HASH"):
        raise ValueError("Node source checksum mismatch")
    args.output.mkdir(parents=True)
    bundle = args.output / f"nexus-openwrt-{args.version}-openwrt25.12.4-x86_64"
    bundle.mkdir()
    for item in packages.values():
        shutil.copyfile(feed / item["file"], bundle / item["file"])
    shutil.copyfile(args.public_key, bundle / "nexus-release-public.pem")
    shutil.copyfile(args.build_config, bundle / "build.config")
    licenses = bundle / "licenses"
    licenses.mkdir()
    for name in ("LICENSE", "NOTICE", "THIRD_PARTY.md"):
        shutil.copyfile(ROOT / name, licenses / name)
    shutil.copyfile(ROOT / "feed/nexus-node-runtime/COPYING", licenses / "node-recipe-COPYING")
    with tarfile.open(node_source, "r:gz") as archive:
        member = archive.getmember(f"node-v{node_version}/LICENSE")
        if not member.isfile() or member.size > 2_000_000:
            raise ValueError("Invalid Node license")
        (licenses / "Node-LICENSE").write_bytes(archive.extractfile(member).read())
    feed_revisions = {}
    for name in ("base", "packages", "luci"):
        feed_revisions[name] = run("git", "-C", args.sdk / "feeds" / name, "rev-parse", "HEAD").decode().strip()
    metadata = {
        "schema_version": 1, "release": args.version,
        "target": {"openwrt": "25.12.4", "subtarget": "x86/64", "apk_architecture": "x86_64"},
        "source_commit": commit,
        "source_url": f"https://github.com/Nexilume-AI/nexus-openwrt/tree/{commit}",
        "sdk_sha256": SDK_SHA256, "feed_revisions": feed_revisions,
        "public_key_sha256": digest(args.public_key), "packages": packages,
        "node_source": {"url": f"https://nodejs.org/dist/v{node_version}/node-v{node_version}.tar.gz", "sha256": digest(node_source)},
        "profiles": ["nexus-agent-router", "nexus-agent-router-relay", "nexus-agent-router-seed"],
        "notes": ["Official OpenWrt repositories are required for dependencies.",
                  "This is a package feed, not a firmware or desktop image.",
                  "Cloud Relay client and self-hosted Open Mesh Relay are separate roles."],
    }
    (bundle / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    shutil.copyfile(ROOT / "docs/package-install.md", bundle / "INSTALL.md")
    apk = args.sdk / "staging_dir/host/bin/apk"
    run(apk, "mkndx", "--root", args.sdk, "--keys-dir", args.sdk,
        "--allow-untrusted", "--sign", args.private_key, "--output", bundle / "packages.adb",
        *(item["file"] for item in packages.values()), cwd=bundle)
    run(apk, "--keys-dir", bundle, "verify", bundle / "packages.adb")
    paths = sorted(path for path in bundle.rglob("*") if path.is_file())
    (bundle / "SHA256SUMS").write_text("".join(f"{digest(path)}  {path.relative_to(bundle).as_posix()}\n" for path in paths), encoding="utf-8")
    archive_path = args.output / (bundle.name + ".tar.gz")
    with tarfile.open(archive_path, "w:gz") as archive:
        archive.add(bundle, arcname=bundle.name)
    shutil.copyfile(args.public_key, args.output / "nexus-release-public.pem")
    (args.output / "SHA256SUMS").write_text(f"{digest(archive_path)}  {archive_path.name}\n{digest(args.public_key)}  nexus-release-public.pem\n", encoding="utf-8")
    print(json.dumps({"archive": archive_path.name, "sha256": digest(archive_path), "public_key_sha256": digest(args.public_key), "source_commit": commit}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("sdk", "sdk-archive", "build-config", "private-key", "public-key", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--version", required=True)
    assemble(parser.parse_args())
