// smile CUDA validation core
// Build (Windows): nvcc -O3 -std=c++17 -arch=sm_XX smile_cuda.cu -o smile-gpu.exe
// This is a real 1 ms-window neuron/edge/mana validation path, not a synthetic GEMM stress test.

#include <cuda_runtime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using i32 = int32_t; using i64 = int64_t;

static constexpr int NORMAL=0, MEMORY=1, GIANT=2;
static constexpr int INPUT=0, CENTRAL=1, OUTPUT=2;
static constexpr int HEALTHY=0, IGNORE=1, DORMANT=3, ASLEEP=4, DEAD=5;
[[maybe_unused]] static constexpr int SPAM=2;
static constexpr i64 CAP[3]={20000,40000,120000};
static __host__ __device__ constexpr int line_count(int kind){return kind==NORMAL?20:(kind==MEMORY?40:60);}
static __host__ __device__ constexpr int memory_bytes(int kind){return kind==NORMAL?32:(kind==MEMORY?1024:4096);}
static __host__ __device__ constexpr int cadence_ms(int kind){return kind==NORMAL?10:(kind==MEMORY?15:50);}
static constexpr int MAX_LINES=60;
static constexpr int SIGNAL_RING=21;
static constexpr int TRANSIT_SLOTS=16;
static constexpr int TRANSIT_MS=500;
static constexpr int SYS_MS=50;
static constexpr int MOUTH_COUNT=14;
static constexpr i64 FIRE_STARTUP=1200, FIRE_PER_LINE=300;
static constexpr u32 NEVER=0xffffffffu;
static constexpr int MAX_CODE=1024, MAX_PROGRAMS=16;

#define CUDA_OK(x) do { cudaError_t _e=(x); if(_e!=cudaSuccess){ \
  std::fprintf(stderr,"CUDA error %s:%d: %s\n",__FILE__,__LINE__,cudaGetErrorString(_e)); std::exit(2);} } while(0)

struct EdgeD { u32 dst; u8 line, delay; u16 pad=0; };
struct SignalD { u32 dst; u8 line, bit; u16 pad=0; };
struct NeuronD {
    i64 mana=0, cap=0;
    u64 rng=1, in_bits=0, pending_set=0, pending_clear=0;
    u32 edge_off=0, mem_off=0;
    u32 last_eval=0, last_fire=NEVER, last_input=NEVER, next_eval=0;
    u16 credit=0;
    u8 kind=NORMAL, lobe=CENTRAL, state=HEALTHY, prog=0, is_mouth=0;
    u8 pad[3]={0,0,0};
};
struct CountersD {
    u64 evals=0, fires=0, signals=0, dropped=0, faults=0, mouth_bits=0;
};
struct DeviceView {
    NeuronD* neurons=nullptr; EdgeD* edges=nullptr; u8* memory=nullptr; u32* in_at=nullptr;
    SignalD* signal_ring=nullptr; u32* signal_count=nullptr; u32 signal_cap=0;
    u64* pools=nullptr; u64* targets=nullptr; u64* cap_sums=nullptr; u64* transit=nullptr;
    u8* output_bits=nullptr; u32* output_count=nullptr; u32 output_cap=0;
    CountersD* counters=nullptr; u32* tick=nullptr; int n=0; int temperature=100;
};

// Same compact VM opcodes as the CPU prototype.
enum Op : u8 { OP_NOP=0,OP_IMM,OP_MOV,OP_ADD,OP_SUB,OP_MUL,OP_DIV,OP_MOD,
 OP_AND,OP_OR,OP_XOR,OP_NOT,OP_SHL,OP_SHR,OP_EQ,OP_LT,OP_GT,OP_SEL,
 OP_LD,OP_ST,OP_SENSE,OP_FIRE,OP_SLEEP,OP_LOOP,OP_ENDL,OP_JMP,OP_JZ,OP_JNZ,OP_HALT };
enum Sense : u16 { SN_INPOP=0,SN_INBITS,SN_FRESHPOP,SN_MANA,SN_MANAPCT,SN_TSF,SN_TSI,SN_NOISE,SN_POOLPCT,SN_LINE0=64 };
static inline u32 enc(u8 op,u8 d,u8 a,u16 im){return ((u32)op<<24)|((u32)(d&15)<<20)|((u32)(a&15)<<16)|im;}
static inline u16 IM(i32 v){return (u16)(0x8000|(v&0x7fff));} static inline u16 RG(int r){return (u16)(r&15);}

__constant__ u32 C_CODE[MAX_CODE];
__constant__ u16 C_OFF[MAX_PROGRAMS], C_LEN[MAX_PROGRAMS];

struct RngH { u64 s; explicit RngH(u64 x=1):s(x?x:1){} u64 next(){u64 z=(s+=0x9e3779b97f4a7c15ull);z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;z=(z^(z>>27))*0x94d049bb133111ebull;return z^(z>>31);} u32 below(u32 n){return n?(u32)(next()>>32)%n:0;} };

static std::vector<u32> seed_normal(int th,int width,int greed){
 std::vector<u32> c; auto P=[&](u8 o,u8 d,u8 a,u16 x){c.push_back(enc(o,d,a,x));};
 P(OP_SENSE,1,0,SN_INPOP);P(OP_SENSE,2,0,SN_FRESHPOP);P(OP_SENSE,3,0,SN_NOISE);P(OP_SENSE,4,0,SN_MANAPCT);P(OP_SENSE,5,0,SN_TSF);P(OP_SENSE,6,0,SN_INBITS);
 P(OP_MUL,7,1,IM(40));P(OP_MUL,8,2,IM(55));P(OP_ADD,7,7,RG(8));P(OP_DIV,8,3,IM(18));P(OP_ADD,7,7,RG(8));P(OP_DIV,8,5,IM(120));P(OP_ADD,7,7,RG(8));
 P(OP_GT,9,7,IM(th));P(OP_GT,10,4,IM(greed));P(OP_AND,9,9,RG(10));P(OP_IMM,11,0,IM(width));P(OP_GT,12,7,IM(th*2));P(OP_SHL,13,11,IM(2));P(OP_OR,13,13,RG(11));P(OP_SEL,11,12,(u16)((13<<4)|11));
 P(OP_MOD,14,3,IM(20));P(OP_SHL,12,11,RG(14));P(OP_IMM,13,0,IM(20));P(OP_SUB,13,13,RG(14));P(OP_SHR,13,11,RG(13));P(OP_OR,11,12,RG(13));P(OP_SHL,12,11,IM(6));P(OP_OR,11,11,RG(12));P(OP_SEL,11,9,(u16)((11<<4)|0));
 P(OP_MOV,15,3,RG(0));P(OP_SHL,12,3,IM(7));P(OP_XOR,15,15,RG(12));P(OP_SHL,12,3,IM(13));P(OP_XOR,15,15,RG(12));P(OP_XOR,15,15,RG(6));P(OP_FIRE,0,11,RG(15));P(OP_HALT,0,0,0);return c;
}
static std::vector<u32> seed_memory(int th){
 std::vector<u32> c; auto P=[&](u8 o,u8 d,u8 a,u16 x){c.push_back(enc(o,d,a,x));};
 P(OP_SENSE,1,0,SN_INPOP);P(OP_SENSE,2,0,SN_FRESHPOP);P(OP_SENSE,3,0,SN_NOISE);P(OP_SENSE,4,0,SN_MANAPCT);P(OP_SENSE,6,0,SN_INBITS);
 P(OP_LD,5,0,IM(0));P(OP_ADD,5,5,IM(1));P(OP_MOD,5,5,IM(15));P(OP_ST,5,0,IM(0));P(OP_ADD,7,5,IM(1));P(OP_ST,1,7,RG(0));
 P(OP_IMM,8,0,IM(0));P(OP_IMM,9,0,IM(1));P(OP_LOOP,0,0,IM(8));P(OP_LD,10,9,RG(0));P(OP_ADD,8,8,RG(10));P(OP_ADD,9,9,IM(1));P(OP_ENDL,0,0,0);
 P(OP_MUL,7,8,IM(6));P(OP_MUL,10,2,IM(30));P(OP_ADD,7,7,RG(10));P(OP_DIV,10,3,IM(8));P(OP_ADD,7,7,RG(10));P(OP_GT,11,7,IM(th));P(OP_GT,12,4,IM(20));P(OP_AND,11,11,RG(12));P(OP_IMM,13,0,IM(0x3f));P(OP_SEL,13,11,(u16)((0<<4)|13));P(OP_SENSE,15,0,SN_NOISE);P(OP_XOR,14,6,RG(8));P(OP_XOR,14,14,RG(15));P(OP_FIRE,0,13,RG(14));P(OP_HALT,0,0,0);return c;
}

__device__ __forceinline__ u64 rng_next(u64& s){u64 z=(s+=0x9e3779b97f4a7c15ull);z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;z=(z^(z>>27))*0x94d049bb133111ebull;return z^(z>>31);}
__device__ __forceinline__ i32 rd(u16 im,i32* r){if(!(im&0x8000))return r[im&15];i32 v=im&0x7fff;if(v&0x4000)v-=0x8000;return v;}
struct VmD {u64 mask=0,bits=0;int fired=0,fault=0,sleep=0;};

__device__ VmD run_vm(DeviceView v,int id,u32 now){
 NeuronD& n=v.neurons[id]; VmD out; i32 r[16]={0}; int nl=line_count(n.kind), inpop=__popcll(n.in_bits),fresh=0;
 for(int i=0;i<nl;i++){u32 t=v.in_at[(size_t)id*MAX_LINES+i];if(t!=NEVER&&now-t<50)fresh++;}
 int manapct=n.cap>0?(int)(n.mana*100/n.cap):0; int poolpct=v.targets[n.lobe]?(int)(v.pools[n.lobe]*100/v.targets[n.lobe]):0;
 int tsf=n.last_fire==NEVER?30000:(int)min((u32)30000,now-n.last_fire); int tsi=n.last_input==NEVER?30000:(int)min((u32)30000,now-n.last_input);
 int off=C_OFF[n.prog],len=C_LEN[n.prog],pc=0,fuel=0,loop_pc=-1,loop_n=0,loop_i=0; int cap=n.kind==NORMAL?64:512; bool loop=n.kind!=NORMAL;
 while(pc>=0&&pc<len&&fuel<cap){fuel++;u32 ins=C_CODE[off+pc++];u8 op=ins>>24,d=(ins>>20)&15,a=(ins>>16)&15;u16 im=ins;
  switch(op){case OP_NOP:break;case OP_IMM:r[d]=rd(im,r);break;case OP_MOV:r[d]=r[a];break;case OP_ADD:r[d]=r[a]+rd(im,r);break;case OP_SUB:r[d]=r[a]-rd(im,r);break;case OP_MUL:r[d]=(i32)(((i64)r[a]*rd(im,r))&0x7fffffff);break;case OP_DIV:{i32 x=rd(im,r);r[d]=x?r[a]/x:0;break;}case OP_MOD:{i32 x=rd(im,r);r[d]=x?r[a]%x:0;break;}
  case OP_AND:r[d]=r[a]&rd(im,r);break;case OP_OR:r[d]=r[a]|rd(im,r);break;case OP_XOR:r[d]=r[a]^rd(im,r);break;case OP_NOT:r[d]=~r[a];break;case OP_SHL:r[d]=(i32)((u32)r[a]<<(rd(im,r)&31));break;case OP_SHR:r[d]=(i32)((u32)r[a]>>(rd(im,r)&31));break;case OP_EQ:r[d]=r[a]==rd(im,r);break;case OP_LT:r[d]=r[a]<rd(im,r);break;case OP_GT:r[d]=r[a]>rd(im,r);break;case OP_SEL:r[d]=r[a]?r[(im>>4)&15]:r[im&15];break;
  case OP_LD:{int sz=memory_bytes(n.kind),p=(r[a]+(im&0x7fff))%sz;if(p<0)p+=sz;r[d]=v.memory[n.mem_off+p];break;}case OP_ST:{int sz=memory_bytes(n.kind),p=(r[a]+(im&0x7fff))%sz;if(p<0)p+=sz;v.memory[n.mem_off+p]=(u8)r[d];break;}
  case OP_SENSE:{u16 ch=im&0x7fff;i32 x=0;if(ch>=SN_LINE0){int li=ch-SN_LINE0;x=li<nl?((n.in_bits>>li)&1):0;}else switch(ch){case SN_INPOP:x=inpop;break;case SN_INBITS:x=(i32)n.in_bits;break;case SN_FRESHPOP:x=fresh;break;case SN_MANA:x=n.mana/1000;break;case SN_MANAPCT:x=manapct;break;case SN_TSF:x=tsf;break;case SN_TSI:x=tsi;break;case SN_POOLPCT:x=poolpct;break;case SN_NOISE:x=(i32)(((rng_next(n.rng)>>32)&255)*v.temperature/255);break;}r[d]=x;break;}
  case OP_FIRE:{u64 m=(u32)r[a];if(nl<64)m&=((1ull<<nl)-1);if(m){out.fired=1;out.mask=m;out.bits=(u32)rd(im,r);}break;}case OP_SLEEP:out.sleep=1;pc=-1;break;
  case OP_LOOP:if(loop){loop_pc=pc;loop_n=min(rd(im,r),8);loop_i=0;if(loop_n<=0){int dep=1;while(pc<len&&dep){u8 x=C_CODE[off+pc]>>24;if(x==OP_LOOP)dep++;if(x==OP_ENDL)dep--;pc++;}loop_pc=-1;}break;}case OP_ENDL:if(loop){if(loop_pc>=0&&++loop_i<loop_n)pc=loop_pc;else loop_pc=-1;}break;case OP_HALT:pc=-1;break;default:break;}
 }
 if(fuel>=cap)out.fault=1;return out;
}

__device__ u64 atomic_take(u64* p,u64 want){u64 old=*p;while(old){u64 take=min(old,want);u64 prev=atomicCAS((unsigned long long*)p,old,old-take);if(prev==old)return take;old=prev;}return 0;}

__global__ void deliver_kernel(DeviceView v){u32 now=*v.tick,slot=now%SIGNAL_RING,count=min(v.signal_count[slot],v.signal_cap);for(u32 i=blockIdx.x*blockDim.x+threadIdx.x;i<count;i+=gridDim.x*blockDim.x){SignalD s=v.signal_ring[(size_t)slot*v.signal_cap+i];if(s.dst>=(u32)v.n||s.line>=MAX_LINES)continue;u64 bit=1ull<<s.line;if(s.bit)atomicOr((unsigned long long*)&v.neurons[s.dst].pending_set,bit);else atomicOr((unsigned long long*)&v.neurons[s.dst].pending_clear,bit);atomicExch(&v.in_at[(size_t)s.dst*MAX_LINES+s.line],now);atomicMax(&v.neurons[s.dst].last_input,now);}}
__global__ void clear_current_kernel(DeviceView v){if(!blockIdx.x&&!threadIdx.x)v.signal_count[*v.tick%SIGNAL_RING]=0;}

__global__ void eval_kernel(DeviceView v){int id=blockIdx.x*blockDim.x+threadIdx.x;if(id>=v.n)return;u32 now=*v.tick;NeuronD& n=v.neurons[id];if(n.kind==GIANT||n.state==DEAD||now<n.next_eval)return;atomicAdd((unsigned long long*)&v.counters->evals,1ull);
 u64 sets=atomicExch((unsigned long long*)&n.pending_set,0ull),clears=atomicExch((unsigned long long*)&n.pending_clear,0ull);n.in_bits=(n.in_bits|sets)&~clears;
 u32 dt=now-n.last_eval;n.last_eval=now;i64 upkeep=n.cap*20*(i64)dt/1000000;if(n.state==DORMANT)upkeep/=10;n.mana-=upkeep;if(upkeep>0){int sl=((now+TRANSIT_MS)/SYS_MS)%TRANSIT_SLOTS;atomicAdd((unsigned long long*)&v.transit[n.lobe*TRANSIT_SLOTS+sl],(u64)upkeep);}
 if(n.mana<n.cap){u64 want=n.cap-n.mana;u64 share=15+min(85,(int)n.credit/180);u64 got=atomic_take(&v.pools[n.lobe],want*share/100);n.mana+=(i64)got;}
 u32 decay=(u32)n.credit*dt/5000;n.credit=n.credit>decay?n.credit-decay:0;if(n.mana<=0){n.mana=0;n.state=IGNORE;}else if(n.mana*100<n.cap*20)n.state=IGNORE;else if(n.state==IGNORE)n.state=HEALTHY;
 int cadence=cadence_ms(n.kind);if(n.state==IGNORE||n.state==ASLEEP){n.next_eval=now+cadence;return;}if(n.last_input!=NEVER&&now-n.last_input>30000&&n.last_fire!=NEVER&&now-n.last_fire>30000)n.state=DORMANT;if(n.state==DORMANT)cadence*=10;
 VmD r=run_vm(v,id,now);if(r.fault){n.state=ASLEEP;atomicAdd((unsigned long long*)&v.counters->faults,1ull);n.next_eval=now+cadence*4;return;}if(r.sleep){n.next_eval=now+cadence*3;return;}
 int refractory=n.is_mouth?600:40;bool blocked=n.last_fire!=NEVER&&now-n.last_fire<(u32)refractory;if(r.fired&&r.mask&&!blocked){i64 cost=FIRE_STARTUP+FIRE_PER_LINE*__popcll(r.mask);if(n.mana>=cost){n.mana-=cost;n.last_fire=now;atomicAdd((unsigned long long*)&v.counters->fires,1ull);n.credit=(u16)min(65535,(int)n.credit+(int)(cost/16));int sl=((now+TRANSIT_MS)/SYS_MS)%TRANSIT_SLOTS;atomicAdd((unsigned long long*)&v.transit[n.lobe*TRANSIT_SLOTS+sl],(u64)cost);
   int nl=line_count(n.kind);u32 ec=(id+1<v.n?v.neurons[id+1].edge_off:(u32)0); // overwritten below for final neuron
   ec=(id==v.n-1)?0:ec-n.edge_off; // final neuron is a giant and has zero edges in this test
   for(u32 j=0;j<ec;j++){EdgeD e=v.edges[n.edge_off+j];int li=e.line%nl;if((r.mask>>li)&1){u32 slot=(now+e.delay)%SIGNAL_RING,at=atomicAdd(&v.signal_count[slot],1u);if(at<v.signal_cap){v.signal_ring[(size_t)slot*v.signal_cap+at]={e.dst,e.line,(u8)((r.bits>>li)&1),0};atomicAdd((unsigned long long*)&v.counters->signals,1ull);}else atomicAdd((unsigned long long*)&v.counters->dropped,1ull);}}
   if(n.is_mouth){int l0=nl-2,l1=nl-1;if(((r.mask>>l0)&1)||((r.mask>>l1)&1)){u32 at=atomicAdd(v.output_count,2u);if(at+1<v.output_cap){v.output_bits[at]=(r.bits>>l0)&1;v.output_bits[at+1]=(r.bits>>l1)&1;atomicAdd((unsigned long long*)&v.counters->mouth_bits,2ull);}}}n.in_bits=0;}}
 u32 jitter=(u32)(rng_next(n.rng)>>32)%(u32)max(1,cadence/2);n.next_eval=now+cadence/2+jitter;}

__global__ void economy_kernel(DeviceView v){if(blockIdx.x||threadIdx.x)return;u32 now=*v.tick;if(now%SYS_MS)return;int sl=(now/SYS_MS)%TRANSIT_SLOTS;for(int l=0;l<3;l++){u64 back=atomicExch((unsigned long long*)&v.transit[l*TRANSIT_SLOTS+sl],0ull);u64 pool=v.pools[l]+back;u64 target=v.targets[l],subs=(u64)(target*(l==INPUT?45:25)/100);if(pool<subs){u64 income=v.cap_sums[l]*260ull*SYS_MS/(20ull*1000ull*1000ull);pool=min(subs,pool+income);}v.pools[l]=min(target,pool);}}
__global__ void advance_kernel(DeviceView v){if(!blockIdx.x&&!threadIdx.x)(*v.tick)++;}

static void enqueue_tick(cudaStream_t s,DeviceView v){deliver_kernel<<<128,256,0,s>>>(v);clear_current_kernel<<<1,1,0,s>>>(v);eval_kernel<<<(v.n+255)/256,256,0,s>>>(v);economy_kernel<<<1,1,0,s>>>(v);advance_kernel<<<1,1,0,s>>>(v);}

// NVML is loaded dynamically: no SDK header or import library is required.
struct Nvml {
 using Dev=void*; struct Util{unsigned int gpu,memory;};
 void* lib=nullptr; Dev dev=nullptr; int ok=0;
 int (*init)()=nullptr;int(*shutdown)()=nullptr;int(*getdev)(unsigned int,Dev*)=nullptr;int(*getutil)(Dev,Util*)=nullptr;int(*gettemp)(Dev,unsigned int,unsigned int*)=nullptr;
 void* sym(const char* n){
#ifdef _WIN32
  return (void*)GetProcAddress((HMODULE)lib,n);
#else
  return dlsym(lib,n);
#endif
 }
 bool open(int index){
#ifdef _WIN32
  lib=(void*)LoadLibraryA("nvml.dll");
#else
  lib=dlopen("libnvidia-ml.so.1",RTLD_LAZY);
#endif
  if(!lib)return false;init=(int(*)())sym("nvmlInit_v2");if(!init)init=(int(*)())sym("nvmlInit");shutdown=(int(*)())sym("nvmlShutdown");getdev=(int(*)(unsigned int,Dev*))sym("nvmlDeviceGetHandleByIndex_v2");if(!getdev)getdev=(int(*)(unsigned int,Dev*))sym("nvmlDeviceGetHandleByIndex");getutil=(int(*)(Dev,Util*))sym("nvmlDeviceGetUtilizationRates");gettemp=(int(*)(Dev,unsigned int,unsigned int*))sym("nvmlDeviceGetTemperature");ok=init&&getdev&&getutil&&!init()&&!getdev(index,&dev);return ok;}
 int sample(int& temp){if(!ok)return -1;Util u{};if(getutil(dev,&u))return -1;unsigned int t=0;if(gettemp&&!gettemp(dev,0,&t))temp=(int)t;return (int)u.gpu;}
 ~Nvml(){if(ok&&shutdown)shutdown();
#ifdef _WIN32
  if(lib)FreeLibrary((HMODULE)lib);
#else
  if(lib)dlclose(lib);
#endif
 }
};

static std::atomic<bool> RUN{true}; static void on_sig(int){RUN=false;}
template<class T> static T* gpu_copy(const std::vector<T>& h){T* p=nullptr;CUDA_OK(cudaMalloc((void**)&p,h.size()*sizeof(T)));if(!h.empty())CUDA_OK(cudaMemcpy(p,h.data(),h.size()*sizeof(T),cudaMemcpyHostToDevice));return p;}
template<class T> static T* gpu_zero(size_t n){T* p=nullptr;CUDA_OK(cudaMalloc((void**)&p,n*sizeof(T)));CUDA_OK(cudaMemset(p,0,n*sizeof(T)));return p;}

int main(int argc,char**argv){int N=32000,limit=70,seconds=120,device=0,batch=100;u64 seed=12345;for(int i=1;i<argc;i++){std::string a=argv[i];auto val=[&](){return i+1<argc?argv[++i]:"0";};if(a=="--neurons")N=std::atoi(val());else if(a=="--gpu-limit")limit=std::atoi(val());else if(a=="--seconds")seconds=std::atoi(val());else if(a=="--device")device=std::atoi(val());else if(a=="--batch")batch=std::atoi(val());else if(a=="--seed")seed=std::strtoull(val(),nullptr,10);else if(a=="--help"){std::puts("smile-gpu [--neurons 32000] [--gpu-limit 70] [--seconds 120] [--device 0] [--batch 100]");return 0;}}
 N=std::max(1000,N);limit=std::max(10,std::min(70,limit));batch=std::max(10,std::min(1000,batch));CUDA_OK(cudaSetDevice(device));cudaDeviceProp prop{};CUDA_OK(cudaGetDeviceProperties(&prop,device));
 std::printf("\n  smile CUDA validation\n  GPU: %s | compute %d.%d | target ceiling %d%%\n",prop.name,prop.major,prop.minor,limit);std::printf("  IMPORTANT: 32k is fixed real work; utilization may stay below the ceiling.\n\n");

 // Programs.
 std::vector<std::vector<u32>> programs;for(int t=0;t<6;t++)programs.push_back(seed_normal(70+t*22,1<<(t%4),18+t*4));for(int t=0;t<3;t++)programs.push_back(seed_memory(90+t*40));programs.push_back({enc(OP_HALT,0,0,0)});
 std::vector<u32> flat;u16 off[MAX_PROGRAMS]={},len[MAX_PROGRAMS]={};for(size_t i=0;i<programs.size();i++){off[i]=(u16)flat.size();len[i]=(u16)programs[i].size();flat.insert(flat.end(),programs[i].begin(),programs[i].end());}if(flat.size()>MAX_CODE){std::fprintf(stderr,"program table too large\n");return 2;}CUDA_OK(cudaMemcpyToSymbol(C_CODE,flat.data(),flat.size()*4));CUDA_OK(cudaMemcpyToSymbol(C_OFF,off,sizeof(off)));CUDA_OK(cudaMemcpyToSymbol(C_LEN,len,sizeof(len)));

 int nmem=std::max(1,(int)std::llround(N*.03)),ngiant=std::max(1,nmem/20),nnorm=N-nmem-ngiant;RngH rng(seed);std::vector<NeuronD> neurons(N);std::vector<u8> memory;memory.reserve((size_t)nnorm*32+(size_t)nmem*1024+(size_t)ngiant*4096);u64 capsum[3]={0,0,0};
 auto setup=[&](int id,int kind,int lobe,int prog){NeuronD& n=neurons[id];n.kind=kind;n.lobe=lobe;n.prog=prog;n.cap=CAP[kind];n.mana=n.cap/2;n.rng=rng.next();n.mem_off=(u32)memory.size();memory.resize(memory.size()+memory_bytes(kind));n.next_eval=rng.below(cadence_ms(kind)+1);capsum[lobe]+=n.cap;};
 int id=0;for(int i=0;i<nnorm;i++,id++){double u=(double)i/std::max(1,nnorm);int l=u<.2?INPUT:(u<.8?CENTRAL:OUTPUT);setup(id,NORMAL,l,rng.below(6));}for(int i=0;i<nmem;i++,id++){double u=(double)i/std::max(1,nmem);int l=u<.15?INPUT:(u<.85?CENTRAL:OUTPUT);setup(id,MEMORY,l,6+rng.below(3));}for(int i=0;i<ngiant;i++,id++)setup(id,GIANT,CENTRAL,9);
 std::vector<int> mouths;for(int i=0;i<N;i++)if(neurons[i].lobe==OUTPUT&&neurons[i].kind!=GIANT)mouths.push_back(i);for(int k=0;k<MOUTH_COUNT&&!mouths.empty();k++){int j=rng.below((u32)mouths.size());neurons[mouths[j]].is_mouth=1;mouths.erase(mouths.begin()+j);}
 std::vector<EdgeD> edges;edges.reserve((size_t)nnorm*20+(size_t)nmem*40);for(int i=0;i<N;i++){neurons[i].edge_off=(u32)edges.size();int ec=neurons[i].kind==NORMAL?20:(neurons[i].kind==MEMORY?40:0);for(int e=0;e<ec;e++){u32 dst;do{dst=rng.below((u32)(N-ngiant));}while(dst==(u32)i);edges.push_back({dst,(u8)rng.below(line_count(neurons[dst].kind)),(u8)(1+rng.below(20)),0});}}
 // Sentinel edge offset makes per-neuron edge count available without another array.
 neurons.push_back(NeuronD{});neurons.back().edge_off=(u32)edges.size();
 std::vector<u32> inat((size_t)N*MAX_LINES,NEVER);std::vector<u64> hpools(3),htargets(3),hcaps(3);for(int l=0;l<3;l++){hcaps[l]=capsum[l];htargets[l]=capsum[l]*3/5;hpools[l]=htargets[l]/2;}

 DeviceView v;v.n=N;v.temperature=100;v.neurons=gpu_copy(neurons);v.edges=gpu_copy(edges);v.memory=gpu_copy(memory);v.in_at=gpu_copy(inat);v.signal_cap=std::max(65536,N*4);v.signal_ring=gpu_zero<SignalD>((size_t)SIGNAL_RING*v.signal_cap);v.signal_count=gpu_zero<u32>(SIGNAL_RING);v.pools=gpu_copy(hpools);v.targets=gpu_copy(htargets);v.cap_sums=gpu_copy(hcaps);v.transit=gpu_zero<u64>(3*TRANSIT_SLOTS);v.output_cap=1u<<20;v.output_bits=gpu_zero<u8>(v.output_cap);v.output_count=gpu_zero<u32>(1);v.counters=gpu_zero<CountersD>(1);v.tick=gpu_zero<u32>(1);
 size_t freeb=0,totalb=0;CUDA_OK(cudaMemGetInfo(&freeb,&totalb));std::printf("  neurons: %d (normal %d, memory %d, CPU giants %d) | edges: %zu\n",N,nnorm,nmem,ngiant,edges.size());std::printf("  VRAM after allocation: %.1f MiB used / %.1f MiB total\n\n",(totalb-freeb)/1048576.0,totalb/1048576.0);

 cudaStream_t stream;CUDA_OK(cudaStreamCreate(&stream));cudaGraph_t graph=nullptr;cudaGraphExec_t exec=nullptr;bool graphed=false;if(cudaStreamBeginCapture(stream,cudaStreamCaptureModeGlobal)==cudaSuccess){enqueue_tick(stream,v);if(cudaStreamEndCapture(stream,&graph)==cudaSuccess&&cudaGraphInstantiate(&exec,graph,nullptr,nullptr,0)==cudaSuccess)graphed=true;}if(!graphed){cudaGetLastError();std::puts("  CUDA Graph unavailable; using ordered stream launches.");}else std::puts("  CUDA Graph active (one exact 1 ms dependency window per launch).");
 Nvml nvml;bool has_nvml=nvml.open(device);std::printf("  NVML utilization feedback: %s\n",has_nvml?"active":"unavailable; using wall-duty ceiling");std::puts("  Press Ctrl+C to stop.\n");std::signal(SIGINT,on_sig);
 using Clock=std::chrono::steady_clock;auto start=Clock::now(),lastprint=start;double duty=limit/100.0;int util=-1,temp=0;CountersD c{};u32 tick=0;u64 giant_steps=0;
 while(RUN){if(seconds>0&&std::chrono::duration<double>(Clock::now()-start).count()>=seconds)break;auto busy0=Clock::now();for(int k=0;k<batch;k++){if(graphed)CUDA_OK(cudaGraphLaunch(exec,stream));else enqueue_tick(stream,v);}CUDA_OK(cudaStreamSynchronize(stream));auto busy1=Clock::now();CUDA_OK(cudaMemcpy(&tick,v.tick,4,cudaMemcpyDeviceToHost));CUDA_OK(cudaMemcpy(&c,v.counters,sizeof(c),cudaMemcpyDeviceToHost));u32 outn=0;CUDA_OK(cudaMemcpy(&outn,v.output_count,4,cudaMemcpyDeviceToHost));CUDA_OK(cudaMemset(v.output_count,0,4));giant_steps=(u64)ngiant*tick/50;
  auto now=Clock::now();double since=std::chrono::duration<double>(now-lastprint).count();if(since>=.5){int t=0;int u=nvml.sample(t);if(u>=0){util=u;temp=t;if(util>limit+2)duty=std::max(.05,duty*(double)limit/std::max(1,util));else if(util<limit-4)duty=std::min(1.0,duty+.04);}if(temp>=85)duty=std::min(duty,.25);double wall=std::chrono::duration<double>(now-start).count(),virt=tick/1000.0,fhz=virt>0?c.fires/virt/std::max(1,N-ngiant):0;std::printf("  wall %6.1fs | virtual %8.1fs (%6.1fx) | GPU %3d%%/%d%% duty %3.0f%% | %2dC | fire %.2f Hz | signals %.1fM | drop %llu\n",wall,virt,virt/std::max(.001,wall),util,limit,duty*100,temp,fhz,c.signals/1e6,(unsigned long long)c.dropped);std::fflush(stdout);lastprint=now;if(temp>=90){std::puts("  thermal stop: GPU reached 90C");break;}}
  double busy=std::chrono::duration<double>(busy1-busy0).count();if(!has_nvml)duty=limit/100.0;double sleep=busy*(1.0-duty)/std::max(.01,duty);if(sleep>0)std::this_thread::sleep_for(std::chrono::duration<double>(sleep));
 }
 std::printf("\n  finished: ticks=%u evals=%llu fires=%llu signals=%llu dropped=%llu CPU-giant-steps=%llu\n",tick,(unsigned long long)c.evals,(unsigned long long)c.fires,(unsigned long long)c.signals,(unsigned long long)c.dropped,(unsigned long long)giant_steps);
 if(exec)cudaGraphExecDestroy(exec);if(graph)cudaGraphDestroy(graph);cudaStreamDestroy(stream);cudaFree(v.neurons);cudaFree(v.edges);cudaFree(v.memory);cudaFree(v.in_at);cudaFree(v.signal_ring);cudaFree(v.signal_count);cudaFree(v.pools);cudaFree(v.targets);cudaFree(v.cap_sums);cudaFree(v.transit);cudaFree(v.output_bits);cudaFree(v.output_count);cudaFree(v.counters);cudaFree(v.tick);return 0;
}
