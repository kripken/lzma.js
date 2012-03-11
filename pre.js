
var LZMA = {
  run: function(data, decompress) {
    var inputIndex = 0;
    var returnValue = [];
    var Module = {
      arguments: ['-q'].concat(decompress ? ['-d'] : []),
      stdin: function() { return inputIndex < data.length ? data[inputIndex++] : null },
      stdout: function(x) { if (x !== null) returnValue.push(x) }
    };


