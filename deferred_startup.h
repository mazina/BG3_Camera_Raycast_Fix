#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Internal one-shot initialization at the existing SDL boundary. No loader,
// companion DLL, worker or heavy initialization runs from Schedule/DllMain.
namespace DeferredStartup {
using InitSDL=int(*)(unsigned);
static InitSDL originalSDL;
static void** initSlot;
static void(*callback)();
static volatile LONG requested,started;
static bool Range(SIZE_T size,SIZE_T offset,SIZE_T bytes){return offset<=size&&bytes<=size-offset;}
static bool Equal(BYTE const*a,char const*b,unsigned n){for(unsigned i=0;i<n;++i)if(a[i]!=BYTE(b[i]))return false;return true;}
// Does not modify the image. Reject missing, ambiguous and out-of-bounds imports.
static void** FindInitSlot(BYTE*base,SIZE_T size){
 if(!Range(size,0,sizeof(IMAGE_DOS_HEADER)))return nullptr;
 auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
 if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||!Range(size,SIZE_T(dos->e_lfanew),sizeof(IMAGE_NT_HEADERS64)))return nullptr;
 auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
 if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||nt->OptionalHeader.SizeOfImage!=size)return nullptr;
 auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
 if(!dir.Size||!Range(size,dir.VirtualAddress,dir.Size))return nullptr;
 void**found=nullptr;bool terminated=false;
 for(SIZE_T offset=0;Range(dir.Size,offset,sizeof(IMAGE_IMPORT_DESCRIPTOR));offset+=sizeof(IMAGE_IMPORT_DESCRIPTOR)){
  auto d=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress+offset);
  if(!d->Name){terminated=true;break;}
  if(!d->OriginalFirstThunk||!d->FirstThunk)return nullptr;
  bool end=false;
  for(SIZE_T i=0;i<size/sizeof(IMAGE_THUNK_DATA64);++i){
   SIZE_T name=SIZE_T(d->OriginalFirstThunk)+i*sizeof(IMAGE_THUNK_DATA64),target=SIZE_T(d->FirstThunk)+i*sizeof(IMAGE_THUNK_DATA64);
   if(!Range(size,name,sizeof(IMAGE_THUNK_DATA64))||!Range(size,target,sizeof(IMAGE_THUNK_DATA64)))return nullptr;
   auto t=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+name);
   if(!t->u1.AddressOfData){end=true;break;}
   if(IMAGE_SNAP_BY_ORDINAL64(t->u1.Ordinal))continue;
   auto text=SIZE_T(t->u1.AddressOfData)+offsetof(IMAGE_IMPORT_BY_NAME,Name);
   if(text<SIZE_T(t->u1.AddressOfData)||!Range(size,text,9))return nullptr;
   if(Equal(base+text,"SDL_Init",9)){
    if(found)return nullptr;found=reinterpret_cast<void**>(base+target);
   }
  }
  if(!end)return nullptr;
 }
 return terminated?found:nullptr;
}
static int Deferred(unsigned flags){
 if(InterlockedCompareExchange(&started,1,0)==0){
  // Unwind only our own import entry. Other independently installed wrappers
  // keep their predecessor and do the same; never overwrite a later hook.
  DWORD old;if(VirtualProtect(initSlot,sizeof(void*),PAGE_READWRITE,&old)){
   InterlockedCompareExchangePointer(initSlot,reinterpret_cast<void*>(originalSDL),reinterpret_cast<void*>(Deferred));DWORD ignored;
   if(!VirtualProtect(initSlot,sizeof(void*),old,&ignored))OutputDebugStringA("Native entry: import protection restoration failed");
  }else OutputDebugStringA("Native entry: import restoration failed; one-shot guard remains active");
  callback();
 }
 return originalSDL(flags);
}
static void Schedule(void(*initialize)()){
 if(InterlockedCompareExchange(&requested,1,0))return;
 callback=initialize;
 auto base=reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
 // The OS-loaded main executable supplies valid PE headers; FindInitSlot then
 // validates every import-table access before an import can be changed.
 if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>1024*1024)return;
 auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);initSlot=FindInitSlot(base,nt->OptionalHeader.SizeOfImage);
 if(!initSlot){OutputDebugStringA("Native entry: SDL_Init import missing, ambiguous or malformed; no hook installed");return;}
 // NativeModLoader runs Init calls serially. Accept an executable predecessor
 // so each installed plugin can defer itself without knowing its peers.
 auto previous=*initSlot;MEMORY_BASIC_INFORMATION info{};
 if(!GetProcAddress(GetModuleHandleW(L"SDL2.dll"),"SDL_Init")||!previous||previous==reinterpret_cast<void*>(Deferred)||
    !VirtualQuery(previous,&info,sizeof(info))||info.State!=MEM_COMMIT||info.Type!=MEM_IMAGE||
    (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
    !(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))){
  OutputDebugStringA("Native entry: SDL_Init predecessor is not an executable image address; no hook installed");return;
 }
 originalSDL=reinterpret_cast<InitSDL>(previous);DWORD old;if(!VirtualProtect(initSlot,sizeof(void*),PAGE_READWRITE,&old))return;
 auto observed=InterlockedCompareExchangePointer(initSlot,reinterpret_cast<void*>(Deferred),previous);DWORD ignored;
 if(!VirtualProtect(initSlot,sizeof(void*),old,&ignored))OutputDebugStringA("Native entry: import protection restoration failed");
 if(observed!=previous)OutputDebugStringA("Native entry: SDL_Init import changed concurrently; no hook installed");
}
}
