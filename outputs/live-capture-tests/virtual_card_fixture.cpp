#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../../components/xband/adapters/ymir/flash_storage.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv){try{
    if(argc==4&&std::wstring_view(argv[1])==L"--synthetic-units"){
        const std::wstring value(argv[2]);
        if(value.empty()||value.find_first_not_of(L"0123456789")!=std::wstring::npos)
            throw std::runtime_error("Units must be an integer0..32767");
        const auto units=std::stoul(value);
        if(units>32767)throw std::runtime_error("Units must be an integer0..32767");
        const std::filesystem::path directory(argv[3]);
        if(!directory.is_absolute()||std::filesystem::exists(directory))
            throw std::runtime_error("Supply a NEW absolute directory; existing cards will not be overwritten");
        std::filesystem::create_directories(directory);
        for(unsigned side=0;side<2;++side){
            std::array<uint8_t,13> image{uint8_t(1+side),2,3,4,5,6,7,uint8_t(side?3:0)};
            unsigned remainder=unsigned(units);
            for(unsigned i=0;i<5;++i){
                const unsigned weight=1u<<(3*(4-i));
                const auto digit=remainder/weight;
                image[8+i]=uint8_t((1u<<digit)-1);remainder%=weight;
            }
            const auto path=directory/(side?L"right-synthetic.bin":L"left-synthetic.bin");
            xband::ymir_adapter::VirtualCardStorage storage(path);storage.save(image);
            std::wcout<<path.c_str()<<L"\n";
        }
        std::cout<<"SYNTHETIC TEST DATA ONLY: "<<units<<" units; new files only; no real-card identity/authentication claim.\n";
        return 0;
    }
    if(argc==4&&std::wstring_view(argv[1])==L"--zero-copy"){
        const std::filesystem::path source(argv[2]),destination(argv[3]);
        if(!source.is_absolute()||!destination.is_absolute()||!std::filesystem::is_regular_file(source)||
           std::filesystem::file_size(source)!=13||std::filesystem::exists(destination)||
           !std::filesystem::is_directory(destination.parent_path()))
            throw std::runtime_error("zero-copy requires an existing absolute 13-byte source and a NEW absolute card file");
        std::array<uint8_t,13> image{};std::ifstream input(source,std::ios::binary);
        if(!input.read(reinterpret_cast<char*>(image.data()),image.size()))throw std::runtime_error("Cannot read source card");
        std::fill(image.begin()+8,image.end(),uint8_t{0});
        xband::ymir_adapter::VirtualCardStorage storage(destination);storage.save(image);
        std::wcout<<destination.c_str()<<L"\n";
        std::cout<<"ZERO-BALANCE TEST COPY ONLY: identity bytes preserved; source unchanged; not inserted automatically.\n";
        return 0;
    }
    if(argc!=3||(std::wstring_view(argv[1])!=L"--synthetic-100"&&std::wstring_view(argv[1])!=L"--synthetic-2"))
        throw std::runtime_error("Usage: virtual-card-fixture --synthetic-units UNITS(0..32767) NEW_ABSOLUTE_DIRECTORY; --synthetic-100|--synthetic-2 NEW_ABSOLUTE_DIRECTORY; --zero-copy SOURCE_CARD NEW_CARD");
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
