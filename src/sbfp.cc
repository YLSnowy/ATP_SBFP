#include "ooo_cpu.h"

void SBFP::extract_and_fill(uint64_t vpage, uint32_t cpu){
    uint64_t block = vpage;
    block = block>>3;
    block = block<<3; //Here, getting the address of cache line which stores adjacent PTEs
    for(int i=0;i<8;i++){
        uint64_t va_pref = block|i;
        //In if condition below, checking if adjacent PTEs are present in page table. It takes 0 latency
        if(va_pref!=vpage && (ooo_cpu[cpu].PTW.page_table.find(va_pref)!=ooo_cpu[cpu].PTW.page_table.end())){ 
            int diff = vpage - va_pref;
            assert(diff>-8 && diff<8 && diff!=0);
            if(fdt[diff+7]>THRESHOLD){ //deciding whether to put free PTE in sampler or PQ
                uint64_t pa_pref = ooo_cpu[cpu].PTW.page_table.find(va_pref)->second;
                insert_PQ(va_pref, pa_pref, diff);
                //cout<<"Inserted in PQ successfully"<<endl;
            }
            else{
                insert_sampler(va_pref, diff);
                //cout<<"Inserted in sampler successfully"<<endl;
            }
        }
    }
}

//Inserting value into Sampler. If Sampler is full, evict one entry in FIFO order and insert recent entry.
void SBFP::insert_sampler(uint64_t vpage, int diff){
    if(sampler.size()<SAMPLER_MAX_SIZE){
        sampler.insert(pair<uint64_t, int> (vpage, diff));
        aux_sampler.push(vpage);
    }
    else{
        sampler.erase(aux_sampler.front());
        aux_sampler.pop();
        insert_sampler(vpage, diff);
    }
}

//Inserting value into PQ. If PQ is full, evict one entry in FIFO order and insert recent entry.
void SBFP::insert_PQ(uint64_t vpage, uint64_t ppage, int diff){
    if(PQ.size()<PQ_MAX_SIZE){
        PQ.insert(pair<uint64_t,pair<uint64_t, int> >(vpage,
                         pair<uint64_t, int>(ppage,diff)));
        aux_PQ.push(vpage);
    }
    else{
        PQ.erase(aux_PQ.front());
        aux_PQ.pop();
        insert_PQ(vpage, ppage, diff);
    }
}

//Function will check if Sampler has the entry of requested virtual page. If yes, update FDT.
void SBFP::check_sampler(uint64_t vpage){
    if(sampler.find(vpage)!=sampler.end()){
        int diff = sampler.find(vpage)->second;
        //cout<<"Hit in sampler"<<endl;
        inc_fdt(diff);
    }
}

//Function will check if PQ has the translation for a virtual page. If yes, update FDT and return physical address.
uint64_t SBFP::check_PQ(uint64_t vpage){
    if(PQ.find(vpage)!=PQ.end()){
        int diff = (PQ.find(vpage)->second).second;
        //cout<<"Hit in PQ"<<endl;
        inc_fdt(diff);
        return (PQ.find(vpage)->second).first;
    }
    return UINT64_MAX;
}

//Update FDTs. If even one FDT saturates, reset all FDTs.
void SBFP::inc_fdt(int diff){
    if(diff==0) return;
    assert(diff>-8 && diff<8);
    fdt[diff+7]++;
    if(fdt[diff+7]>1023) 
        reset_fdt();
}

//Resetting all FDTs
void SBFP::reset_fdt(){
    for(int i=0;i<15;i++){
        if(i==7) continue;
        fdt[i] = fdt[i]>>1;
    }
}

//Returns read latency of PQ
uint64_t SBFP::getLatency(){
    return SBFP_PQ_LATENCY;
}


