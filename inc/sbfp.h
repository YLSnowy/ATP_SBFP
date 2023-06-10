#include<iostream>
#include <queue>
#include <map>
#include <utility>

using namespace std;

//Below values are based on paper.

#define SBFP_PQ_LATENCY 2 

#define PQ_MAX_SIZE 64
#define SAMPLER_MAX_SIZE 64

#define THRESHOLD 100

class SBFP{
    public:
    uint16_t fdt[15];
    map<uint64_t, pair<uint64_t,int>> PQ;
    map<uint64_t, int> sampler;
    queue<uint64_t> aux_PQ, aux_sampler; //Used as support for FIFO implementation in Prefetch Buffer (or PQ) and Sampler

    SBFP(){
        for(int i=0;i<15;i++) fdt[i]=0;
    }

    void extract_and_fill(uint64_t vpage, uint32_t cpu);
    void insert_sampler(uint64_t vpage, int diff);
    void insert_PQ(uint64_t vpage, uint64_t ppage, int diff);
    void check_sampler(uint64_t vpage);
    uint64_t check_PQ(uint64_t vpage);
    void reset_fdt();
    void inc_fdt(int diff);
    uint64_t getLatency();
};
