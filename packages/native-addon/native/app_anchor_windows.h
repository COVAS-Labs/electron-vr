#pragma once
#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#include <cstring>
#include <string>
#include "anchor_transform.h"

namespace electron_vr::anchor {
constexpr uint32_t kAnchorMagic=0x41565245, kAnchorVersion=1;
struct Sample {
  uint32_t magic=kAnchorMagic, version=kAnchorVersion, size=sizeof(Sample), pid=0;
  uint64_t session=0, tick=0;
  uint32_t valid=0, reserved=0;
  Transform stage_from_application;
};
inline std::wstring ChannelName(DWORD pid) { return L"Local\\electron-vr-app-anchor-v1-"+std::to_wstring(pid); }
inline bool Alive(DWORD pid) {
  HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
  if (!process) return false;
  const bool alive=WaitForSingleObject(process,0)==WAIT_TIMEOUT;
  CloseHandle(process); return alive;
}
class Publisher {
 public:
  Publisher()=default;
  Publisher(const Publisher&)=delete;
  ~Publisher() { Invalidate(); if(view_) UnmapViewOfFile(view_); if(mapping_) CloseHandle(mapping_); if(mutex_) CloseHandle(mutex_); }
  void Publish(const Transform& pose,uint64_t session) {
    if (!Valid(pose)) { Invalidate(); return; }
    if (!view_) {
      const auto name=ChannelName(GetCurrentProcessId());
      if(!mutex_)mutex_=CreateMutexW(nullptr,FALSE,(name+L"-lock").c_str());
      if(!mapping_)mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Sample),name.c_str());
      if(mapping_) view_=static_cast<Sample*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(Sample)));
    }
    if(!view_||!mutex_)return;
    const DWORD lock=WaitForSingleObject(mutex_,0);
    if(lock!=WAIT_OBJECT_0 && lock!=WAIT_ABANDONED)return;
    Sample value; value.pid=GetCurrentProcessId(); value.session=session; value.tick=GetTickCount64();
    value.valid=1; value.stage_from_application=pose;
    *view_=value; session_=session;
    ReleaseMutex(mutex_);
  }
  void Invalidate() {
    if(!view_||!mutex_)return;
    const DWORD lock=WaitForSingleObject(mutex_,0);
    if(lock!=WAIT_OBJECT_0 && lock!=WAIT_ABANDONED)return;
    if(view_->session==session_) view_->valid=0;
    ReleaseMutex(mutex_);
  }
 private:
  HANDLE mapping_=nullptr,mutex_=nullptr;
  Sample* view_=nullptr;
  uint64_t session_=0;
};
inline bool ReadProcess(DWORD pid,Sample* sample) {
  const auto name=ChannelName(pid);
  HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
  if(!mapping)return false;
  HANDLE mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,(name+L"-lock").c_str());
  const auto* view=static_cast<const Sample*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(Sample)));
  bool valid=false;
  if(mutex&&view) {
    const DWORD lock=WaitForSingleObject(mutex,0);
    if(lock==WAIT_OBJECT_0 || lock==WAIT_ABANDONED) {
      *sample=*view; ReleaseMutex(mutex);
      const uint64_t now=GetTickCount64();
      valid=sample->magic==kAnchorMagic && sample->version==kAnchorVersion && sample->size==sizeof(Sample) &&
        sample->pid==pid && sample->session && sample->valid==1 && sample->tick<=now && now-sample->tick<2000 && Valid(sample->stage_from_application);
    }
  }
  if(view)UnmapViewOfFile(view); if(mutex)CloseHandle(mutex); CloseHandle(mapping);
  return valid;
}
class Reader {
 public:
  void Reset() { pid_=0; last_scan_=0; }
  DWORD Process() const { return pid_; }
  bool Read(Sample* out) {
    if(pid_ && ReadProcess(pid_,out))return true;
    const uint64_t now=GetTickCount64();
    if(now-last_scan_<1000)return false;
    last_scan_=now;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return false;
    PROCESSENTRY32W process{}; process.dwSize=sizeof(process);
    Sample candidate; unsigned candidates=0;
    if(Process32FirstW(snapshot,&process)) do {
      Sample value;
      if(ReadProcess(process.th32ProcessID,&value)) { candidate=value; ++candidates; }
    } while(Process32NextW(snapshot,&process));
    CloseHandle(snapshot);
    // Never guess between multiple active VR applications.
    if(candidates!=1)return false;
    pid_=candidate.pid; *out=candidate; return true;
  }
 private:
  DWORD pid_=0;
  uint64_t last_scan_=0;
};
}  // namespace electron_vr::anchor
#endif
