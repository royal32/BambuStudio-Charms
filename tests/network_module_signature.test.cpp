#include "slic3r/Utils/CertificateVerify.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
int main(int argc,char** argv)
{
    try {
        if (argc!=4) throw std::runtime_error("Expected official plug-in, private-signed fixture, and temporary folder");
        const fs::path official=argv[1], private_fixture=argv[2], folder=argv[3];
        require(Slic3r::IsOfficialBambuNetworkModule(official.string()),"Official signed Bambu plug-in rejected");
        require(!Slic3r::IsOfficialBambuNetworkModule(private_fixture.string()),"Private signer accepted as Bambu");
        require(!Slic3r::IsOfficialBambuNetworkModule("/usr/bin/true"),"Unrelated Apple code accepted as Bambu");
        require(!Slic3r::IsOfficialBambuNetworkModule((folder/"missing.dylib").string()),"Missing library accepted");
        const auto tampered=folder/"tampered.dylib";
        fs::copy_file(official,tampered,fs::copy_options::none);
        {
            std::fstream file(tampered,std::ios::binary|std::ios::in|std::ios::out);
            file.seekg(4096); char byte=0; file.read(&byte,1);
            require(bool(file),"Cannot read synthetic tampered copy");
            byte^=1; file.seekp(4096); file.write(&byte,1);
            require(bool(file),"Cannot modify synthetic tampered copy");
        }
        require(!Slic3r::IsOfficialBambuNetworkModule(tampered.string()),"Tampered Bambu code accepted");
        fs::remove(tampered);
        std::cout<<"PASS: official Bambu signature accepted; private signer, unrelated Apple code, missing and tampered libraries rejected.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
