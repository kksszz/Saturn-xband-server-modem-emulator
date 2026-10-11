#include "card_runtime.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <windows.h>
#include <nlohmann/json.hpp>
#include "../ymir/flash_storage.hpp"
#include <type_traits>
static_assert(std::is_same_v<xband::ymir_adapter::VirtualCardStorage,xband::windows_storage::VirtualCardStorage>);
static_assert(std::is_same_v<xband::ymir_adapter::FlashStorage,xband::windows_storage::FlashStorage>);
static unsigned checks=0;
static void check(bool b){++checks;if(!b){std::cerr<<"failed card check "<<checks<<'\n';std::exit(1);}}
int main(){
 _putenv_s("XBAND_MEDNAFEN_SIDE", "0");
 const auto root=std::filesystem::temp_directory_path()/
   ("mednafen-card-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
 check(!std::filesystem::exists(root));std::filesystem::create_directory(root);
 const auto path=root/L"card.bin";
 auto* c=xband_card_create(path.c_str());check(c);check(xband_card_units(c)==100);
 check(std::filesystem::file_size(path)==13);check(!xband_card_create(path.c_str()));
 xband_card_write(c,0x8d);xband_card_write(c,0x81);
 const unsigned char fixture[]={1,2,3,4,5,6,7,0,0,0,1,15,15};
 for(unsigned i=0;i<104;++i){
   const auto expected=(fixture[i/8]>>(7-i%8))&1;
   check(xband_card_read(c,0xaa)==uint8_t((0xaa&~0x11u)|0x10u|expected));
   xband_card_write(c,0x85);xband_card_write(c,0x81);
 }
 // Clear the final set bit in byte12 (one unit), persist, reopen, reset pins.
 xband_card_reset(c);
 for(unsigned i=0;i<100;++i){xband_card_write(c,0x85);xband_card_write(c,0x81);}
 xband_card_write(c,0x89);xband_card_write(c,0x81);
 check(xband_card_units(c)==99);check(xband_card_flush(c));xband_card_destroy(c);
 c=xband_card_create(path.c_str());check(c&&xband_card_units(c)==99);
 xband_card_reset(c);check(xband_card_units(c)==99);
 using J=nlohmann::json;
 auto report=[&]{return J::parse(xband_card_report(c));};
 check(report().at("units")==99&&report().at("inserted")==true);
 auto command=[&](J j){const auto wire=j.dump();return xband_card_command(c,wire.c_str());};
 check(command({{"version",1},{"token","off"},{"inserted",false}}));
 check((xband_card_read(c,0xff)&0x11)==0&&xband_card_units(c)==99);
 check(command({{"version",1},{"token","fault"},{"inserted",true},{"read_fault",true}}));
 check(report().at("units").is_null()&&report().at("read_fault")==true);
 check(command({{"version",1},{"token","clear"},{"inserted",true},{"read_fault",false}}));
 check(xband_card_units(c)==99);
 auto fixtureFile=[&](const wchar_t* name,unsigned units){
   auto file=root/name;std::array<uint8_t,13> bytes{9,8,7,6,5,4,3,2};
   for(unsigned i=0;i<5;++i){const auto weight=1u<<(3*(4-i));const auto digit=units/weight;
     bytes[8+i]=uint8_t((1u<<digit)-1);units%=weight;}
   std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),13);out.close();return file;
 };
 auto replacement=fixtureFile(L"fifty.bin",50);
 const auto encoded=replacement.u8string();const auto replacementText=std::string(encoded.begin(),encoded.end());
 const J swap{{"version",1},{"token","swap50"},{"inserted",false},{"image_path",replacementText}};
 check(command(swap));check(report().at("inserted")==false&&xband_card_units(c)==50);
 xband_card_frame(c);check(report().at("inserted")==false);
 xband_card_frame(c);check(report().at("inserted")==true);
 check(command(swap));check(report().at("inserted")==true); // No replay of OFF interval.
 check(!command({{"version",1},{"token","bad-path"},{"inserted",false},{"image_path","relative.bin"}}));
 check(report().at("inserted")==true&&xband_card_units(c)==50&&!report().at("command_error").get<std::string>().empty());
 xband_card_destroy(c);c=xband_card_create(path.c_str());check(c&&xband_card_units(c)==50);
 for(unsigned units:{0,100,32767}){
   const auto file=fixtureFile((std::to_wstring(units)+L".bin").c_str(),units);
   const auto text=file.u8string();
   check(command({{"version",1},{"token",std::to_string(units)},{"inserted",false},{"image_path",std::string(text.begin(),text.end())}}));
   check(xband_card_units(c)==int(units));xband_card_frame(c);xband_card_frame(c);
 }
 xband_card_destroy(c);
 c=xband_card_create(path.c_str());check(c&&xband_card_units(c)==32767);xband_card_destroy(c);
 // Malformed existing file is not replaced with a fresh card.
 const auto bad=root/L"bad.bin";{std::ofstream out(bad,std::ios::binary);out.put('x');}
 check(!xband_card_create(bad.c_str()));check(std::filesystem::file_size(bad)==1);
 check(!xband_card_create(L"relative.bin"));check(xband_card_read(nullptr,0xff)==0xee);
 xband_card_reset(nullptr);xband_card_write(nullptr,0);xband_card_destroy(nullptr);
 _putenv_s("XBAND_MEDNAFEN_SIDE", "1");
 const auto side1=root/L"side1.bin";c=xband_card_create(side1.c_str());check(c&&xband_card_units(c)==100);xband_card_destroy(c);
 std::array<unsigned char,13> side1Bytes{};{std::ifstream in(side1,std::ios::binary);in.read(reinterpret_cast<char*>(side1Bytes.data()),13);}
 check(side1Bytes[0]==2&&side1Bytes[7]==3);
 _putenv_s("XBAND_MEDNAFEN_SIDE", "0");
 // Preserve generated evidence in a uniquely named temp directory.
 std::cout<<checks<<" card runtime checks passed; files: "<<root.string()<<'\n';
}
