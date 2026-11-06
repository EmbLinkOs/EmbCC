; (an %include'd file: definitions a source shares)
%define VERSION_MAJOR 1
%define VERSION_MINOR 4
%define MAKE_VERSION(a, b) ((a) << 8 | (b))
struc frame
    .rip:   resq 1
    .cs:    resq 1
    .flags: resq 1
    .rsp:   resq 1
    .ss:    resq 1
endstruc
