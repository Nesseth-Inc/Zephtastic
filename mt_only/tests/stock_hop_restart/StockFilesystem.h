// SPDX-License-Identifier: GPL-3.0-only
// Only the donor's byte-stream filesystem boundary is simulated here.
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

inline std::vector<uint8_t> donorFile;
inline unsigned donorWrites=0;
inline void checkPath(const char *path){assert(!std::strcmp(path,"/prefs/hopScalingState.bin"));}
struct DonorRead {
    explicit operator bool() const{return !donorFile.empty();}
    size_t read(uint8_t *out,size_t size){
        const size_t n=std::min(size,donorFile.size());
        std::memcpy(out,donorFile.data(),n);return n;
    }
    void close(){}
};
struct DonorFilesystem {
    void mkdir(const char *path){assert(!std::strcmp(path,"/prefs"));}
    DonorRead open(const char *path,int){checkPath(path);return {};}
};
inline DonorFilesystem donorFs;
#define FSCom donorFs
constexpr int FILE_O_READ=1;
struct SafeFile {
    SafeFile(const char *path,bool atomic){checkPath(path);assert(atomic);}
    size_t write(const uint8_t *data,size_t n){donorFile.assign(data,data+n);++donorWrites;return n;}
    bool close(){return true;}
};
inline int spiLock;
namespace concurrency { struct LockGuard {explicit LockGuard(int&){};}; }
