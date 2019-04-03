#!/bin/bash -ex
cmake -H. -Bb -DCMAKE_BUILD_TYPE=Release
cmake --build b --config Release
cd b
ctest -C Release -V
