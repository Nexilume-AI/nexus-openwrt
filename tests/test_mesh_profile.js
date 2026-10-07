const {test}=require('node:test');
const assert=require('node:assert/strict');
const {normalizePaths}=require('../feed/nexus-agent-services/files/mesh-profile');
const path={directory_host:'mesh.example.org',directory_port:28443,relay_host:'2001:db8::1',relay_port:27444};
test('public paths preserve distinct external ports and support DNS IPv4 IPv6',()=>{
  assert.deepEqual(normalizePaths([],path),[path]);
  assert.deepEqual(normalizePaths([path,{...path,relay_host:'192.168.250.1'}]),[path,{...path,relay_host:'192.168.250.1'}]);
});
test('bounded paths reject ambiguous objects, duplicates and unsafe ports',()=>{
  for(const invalid of [null,{},[null],[{...path,password:'secret'}],[path,path],Array(5).fill(path),[{...path,directory_port:'443'}],[{...path,directory_port:0}],[{...path,relay_port:65536}],[{...path,relay_host:'https://seed/'}]])
    assert.throws(()=>normalizePaths(invalid,path));
});
