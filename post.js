
    return new Uint8Array(returnValue);
  },

  compress: function(data) {
    return this.run(data);
  },

  decompress: function(data) {
    return this.run(data, true);
  }
};

