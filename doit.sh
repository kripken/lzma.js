cd lzip

#make clean
#make lzip -j 2 # native build
#mv lzip ../lzma-native

#make clean
~/Dev/emscripten/emmake make lzip -j 2 # bitcode build
mv lzip lzip.bc
 
cd ..

~/Dev/emscripten/emcc -O2 lzip/lzip.bc -o lzma.raw.js
# -s INLINING_LIMIT=0
cat pre.js > lzma.js
cat lzma.raw.js >> lzma.js
cat post.js >> lzma.js

