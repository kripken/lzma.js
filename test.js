// js -m -n -e "load('lzma.js')" test.js

var data = [100, 200, 200, 200, 200, 200, 200, 100, 100, 200, 200, 200, 200, 0, 1];
print('DATA: ' + data);

var compressed = LZMA.compress(data);
print('COMP: ' + compressed + typeof compressed);

var decompressed = LZMA.decompress(compressed);
print('DCMP: ' + decompressed);

