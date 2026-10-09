#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../../components/xband/adapters/ymir/flash_storage.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv){try{
    if(argc!=3||(std::wstring_view(argv[1])!=L"--synthetic-100"&&std::wstring_view(argv[1])!=L"--synthetic-2"))
        throw std::runtime_error("Usage: virtual-card-fixture --synthetic-100|--synthetic-2 NEW_ABSOLUTE_DIRECTORY");
    const bool two=std::wstring_view(argv[1])==L"--synthetic-2";
    const std::filesystem::path directory(argv[2]);
    if(!directory.is_absolute()||std::filesystem::exists(directory))
        throw std::runtime_error("Supply a NEW absolute directory; existing cards will not be overwritten");
    std::filesystem::create_directories(directory);
    std::array<std::array<uint8_t,13>,2> images{{
        {1,2,3,4,5,6,7,0,0,0,1,15,15},
        {2,2,3,4,5,6,7,3,0,0,1,15,15}}};
    if(two)for(auto& image:images){image[8]=image[9]=image[10]=image[11]=0;image[12]=3;}
    for(unsigned side=0;side<2;++side){
        const auto path=directory/(two?(side?L"right-synthetic-2.bin":L"left-synthetic-2.bin"):(side?L"right-synthetic-100.bin":L"left-synthetic-100.bin"));
        xband::ymir_adapter::VirtualCardStorage storage(path);storage.save(images[side]);
        std::wcout<<path.c_str()<<L"\n";
    }
    std::cout<<"SYNTHETIC TEST DATA ONLY: "<<(two?2:100)<<" units, no real-card identity/authentication claim; not inserted automatically.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
