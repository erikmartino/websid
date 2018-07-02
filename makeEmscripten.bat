:: **** use the "-s WASM" switch to compile WebAssembly output. warning: the SINGLE_FILE approach does NOT currently work in Chrome 63.. ****
emcc.bat -s WASM=0 -Os -O3 -s ASSERTIONS=1 -s SAFE_HEAP=0  -s VERBOSE=0 -Wno-pointer-sign --closure 1 --llvm-lto 1 -I./src  --memory-init-file 0  -s NO_FILESYSTEM=1 src/filter.cpp src/envelope.cpp src/sid.cpp src/memory.c src/cpu.c src/hacks.c src/cia.c src/vic.c src/rsidengine.c src/digi.c src/sidplayer.c -s EXPORTED_FUNCTIONS="['_loadSidFile', '_playTune', '_getMusicInfo', '_getSampleRate', '_getSoundBuffer', '_getSoundBufferLen', '_computeAudioSamples', '_enableVoices', '_envIsSID6581', '_envSetSID6581', '_envIsNTSC', '_envSetNTSC', '_getBufferVoice1', '_getBufferVoice2', '_getBufferVoice3', '_getBufferVoice4', '_getRegisterSID', '_getRAM', '_malloc', '_free']" -o htdocs/tinyrsid.js -s SINGLE_FILE=0 -s EXTRA_EXPORTED_RUNTIME_METHODS=['ccall']  -s BINARYEN_ASYNC_COMPILATION=1 -s BINARYEN_TRAP_MODE='clamp' && copy /b shell-pre.js + htdocs\tinyrsid.js + shell-post.js htdocs\tinyrsid3.js && del htdocs\tinyrsid.js && copy /b htdocs\tinyrsid3.js + tinyrsid_adapter.js htdocs\backend_tinyrsid.js && del htdocs\tinyrsid3.js