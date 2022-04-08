# KHR_audio
Implementation of the `KHR_audio` glTF extension in C++.  
  
For Windows, this project depends on Eclipse C++, MSYS2 and the listed libraries.
At a later point of time, this will be resolved by adding a CMake file plus making the required changes to build for different platforms.  
  
This project is a research project to evaluate a possible integration of this extension into [Gestaltor](https://gestaltor.io/).  
  
## Required libraries for MSYS2
`pacman -S mingw-w64-x86_64-glm`  
`pacman -S mingw-w64-x86_64-mpg123`  
`pacman -S mingw-w64-x86_64-nlohmann-json`  
`pacman -S mingw-w64-x86_64-openal`  
  
## References
Eclipse  
https://www.eclipse.org/  
glm  
https://github.com/g-truc/glm  
JSON  
https://github.com/nlohmann/json  
mpg123  
https://www.mpg123.de/  
MSYS2  
https://www.msys2.org/  
KHR_audio  
https://github.com/KhronosGroup/glTF/pull/2137  
OpenAL soft  
https://github.com/kcat/openal-soft  
Quick Sounds  
https://quicksounds.com/  
Web Audio Sound Cones  
https://webaudio.github.io/web-audio-api/#Spatialization-sound-cones  
  