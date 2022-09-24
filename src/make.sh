i586-mingw32msvc-windres resource.rc -O coff -o resource.o
i586-mingw32msvc-g++ ConManager.cpp LunaPort.cpp resource.o -o ../LunaPort.exe -lwsock32 -lwinmm -lcomdlg32 -lm -O3
rm -f resource.o
i586-mingw32msvc-strip -s ../LunaPort.exe
