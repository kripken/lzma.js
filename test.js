// js -m -n -e "load('lzma.js')" test.js

function assertEq(a, b) {
  if (a !== b) {
    throw 'Should have been equal: ' + a + ' : ' + b;
  }
  return false;
}

function assertNeq(a, b) {
  try {
    assertEq(a, b);
  } catch(e) {
    return;
  }
  throw 'Should have not been equal: ' + a + ' : ' + b;
}

function byteCompare(a, b) {
  assertEq(JSON.stringify(new Uint8Array(a)), JSON.stringify(new Uint8Array(b)));
}

var data = [100, 200, 200, 200, 200, 200, 200, 100, 100, 200, 200, 200, 200, 0, 1];
var compressed = LZMA.compress(data);
var decompressed = LZMA.decompress(compressed);

byteCompare(data, decompressed);
assertNeq(data.length, compressed.length);

print('ok.');

