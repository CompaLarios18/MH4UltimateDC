# mh4n_loader: the process loader and the system pages (N1-03).
mh4n_module(
    SOURCES
        elf.cpp
        exheader.cpp
        loader.cpp
        system_pages.cpp
    DEPENDS
        mh4n_memory
    THIRD_PARTY
        mh4n_fmt
)
