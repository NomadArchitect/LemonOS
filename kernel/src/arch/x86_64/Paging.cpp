#include <APIC.h>
#include <CPU.h>
#include <CString.h>
#include <IDT.h>
#include <Logging.h>
#include <Memory.h>
#include <Paging.h>
#include <Panic.h>
#include <PhysicalAllocator.h>
#include <Scheduler.h>
#include <StackTrace.h>
#include <UserPointer.h>

// extern uint32_t kernel_end;

#define KERNEL_HEAP_PDPT_INDEX 511
#define KERNEL_HEAP_PML4_INDEX 511

uint64_t kernelPML4Phys;
extern int lastSyscall;

namespace Memory {
pml4_t kernelPML4 __attribute__((aligned(4096)));
pdpt_t kernelPDPT __attribute__((aligned(4096))); // Kernel itself will reside here (0xFFFFFFFF80000000)
page_dir_t kernelDir __attribute__((aligned(4096)));
page_dir_t kernelHeapDir __attribute__((aligned(4096)));
page_t kernelHeapDirTables[TABLES_PER_DIR][PAGES_PER_TABLE] __attribute__((aligned(4096)));
page_dir_t ioDirs[4] __attribute__((aligned(4096)));

lock_t kernelHeapDirLock = 0;

HashMap<uintptr_t, PageFaultTrap>* pageFaultTraps;

uint64_t VirtualToPhysicalAddress(uint64_t addr) {
    uint64_t address = 0;

    uint32_t pml4Index = PML4_GET_INDEX(addr);
    uint32_t pageDirIndex = PAGE_DIR_GET_INDEX(addr);
    uint32_t pageTableIndex = PAGE_TABLE_GET_INDEX(addr);

    if (pml4Index < 511) { // From Process Address Space

    } else { // From Kernel Address Space
        if (kernelHeapDir[pageDirIndex] & 0x80) {
            address = (GetPageFrame(kernelHeapDir[pageDirIndex])) << 12;
        } else {
            address = (GetPageFrame(kernelHeapDirTables[pageDirIndex][pageTableIndex])) << 12;
        }
    }
    return address;
}

uint64_t VirtualToPhysicalAddress(uint64_t addr, page_map_t* addressSpace) {
    uint64_t address = 0;

    uint32_t pml4Index = PML4_GET_INDEX(addr);
    uint32_t pdptIndex = PDPT_GET_INDEX(addr);
    uint32_t pageDirIndex = PAGE_DIR_GET_INDEX(addr);
    uint32_t pageTableIndex = PAGE_TABLE_GET_INDEX(addr);

    if (pml4Index == 0) { // From Process Address Space
        if ((addressSpace->pageDirs[pdptIndex * 512 + pageDirIndex] & 0x1) &&
            addressSpace->pageTables[pdptIndex][pageDirIndex])
            return addressSpace->pageTables[pdptIndex][pageDirIndex][pageTableIndex] & PAGE_FRAME;
        else
            return 0;
    } else { // From Kernel Address Space
        if (kernelHeapDir[pageDirIndex] & 0x80) {
            address = (GetPageFrame(kernelHeapDir[pageDirIndex])) << 12;
        } else {
            address = (GetPageFrame(kernelHeapDirTables[pageDirIndex][pageTableIndex])) << 12;
        }
    }
    return address;
}

page_table_t AllocatePageTable() {
    void* virt = KernelAllocate4KPages(1);
    uint64_t phys = Memory::AllocatePhysicalMemoryBlock();

    KernelMapVirtualMemory4K(phys, (uintptr_t)virt, 1);

    page_table_t pTable;
    pTable.phys = phys;
    pTable.virt = (page_t*)virt;

    for (int i = 0; i < PAGES_PER_TABLE; i++) {
        ((page_t*)virt)[i] = 0;
    }
    assert(pTable.virt);
    
    return pTable;
}

page_table_t CreatePageTable(uint16_t pdptIndex, uint16_t pageDirIndex, PageMap* pageMap) {
    page_table_t pTable = AllocatePageTable();

    pd_entry_t* entry = &pageMap->pageDirs[pageDirIndex + pdptIndex * 512];
    SetPageFrame(entry, pTable.phys);
    (*entry) |= PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    pageMap->pageTables[pdptIndex][pageDirIndex] = pTable.virt;

    return pTable;
}

void GPFHandler(void*, RegisterContext* regs);
void PageFaultHandler(void*, RegisterContext* regs);

void InitializeVirtualMemory() {
    IDT::RegisterInterruptHandler(13, GPFHandler);
    IDT::RegisterInterruptHandler(14, PageFaultHandler);
    memset(kernelPML4, 0, sizeof(pml4_t));
    memset(kernelPDPT, 0, sizeof(pdpt_t));
    memset(kernelHeapDir, 0, sizeof(page_dir_t));

    SetPageFrame(&(kernelPML4[PML4_GET_INDEX(KERNEL_VIRTUAL_BASE)]), ((uint64_t)kernelPDPT - KERNEL_VIRTUAL_BASE));
    kernelPML4[PML4_GET_INDEX(KERNEL_VIRTUAL_BASE)] |= 0x3;
    kernelPML4[0] = kernelPML4[PML4_GET_INDEX(KERNEL_VIRTUAL_BASE)];

    kernelPDPT[PDPT_GET_INDEX(KERNEL_VIRTUAL_BASE)] = ((uint64_t)kernelDir - KERNEL_VIRTUAL_BASE) | 0x3;
    for (int j = 0; j < TABLES_PER_DIR; j++) {
        kernelDir[j] = (PAGE_SIZE_2M * j) | 0x83;
    }

    kernelPDPT[KERNEL_HEAP_PDPT_INDEX] = 0x3;
    SetPageFrame(&(kernelPDPT[KERNEL_HEAP_PDPT_INDEX]), (uint64_t)kernelHeapDir - KERNEL_VIRTUAL_BASE);

    for (int i = 0; i < 4; i++) {
        kernelPDPT[PDPT_GET_INDEX(IO_VIRTUAL_BASE) + i] =
            ((uint64_t)ioDirs[i] - KERNEL_VIRTUAL_BASE) | 0x3; //(PAGE_SIZE_1G * i) | 0x83;
        for (int j = 0; j < TABLES_PER_DIR; j++) {
            ioDirs[i][j] =
                (PAGE_SIZE_1G * i + PAGE_SIZE_2M * j) | (PDE_2M | PDE_WRITABLE | PDE_PRESENT | PDE_CACHE_DISABLED);
        }
    }

    kernelPDPT[0] =
        kernelPDPT[PDPT_GET_INDEX(KERNEL_VIRTUAL_BASE)]; // Its important that we identity map low memory for SMP

    for (int i = 0; i < TABLES_PER_DIR; i++) {
        memset(&(kernelHeapDirTables[i]), 0, sizeof(page_t) * PAGES_PER_TABLE);
    }

    uint64_t cr0;
    asm volatile("mov %%cr0, %%rax" : "=a"(cr0));
    cr0 |= CR0_WP;
    asm volatile("mov %%rax, %%cr0" :: "a"(cr0));
    

    kernelPML4Phys = (uint64_t)kernelPML4 - KERNEL_VIRTUAL_BASE;
    asm("mov %%rax, %%cr3" ::"a"((uint64_t)kernelPML4 - KERNEL_VIRTUAL_BASE));
}

void LateInitializeVirtualMemory() {
    pageFaultTraps = new HashMap<uintptr_t, PageFaultTrap>();

    RegisterPageFaultTrap(PageFaultTrap{.instructionPointer = reinterpret_cast<uintptr_t>(user_memcpy_trap),
                                        .handler = user_memcpy_trap_handler});
    RegisterPageFaultTrap(PageFaultTrap{.instructionPointer = reinterpret_cast<uintptr_t>(user_strlen_trap),
                                        .handler = user_strlen_trap_handler});
    RegisterPageFaultTrap(PageFaultTrap{.instructionPointer = reinterpret_cast<uintptr_t>(user_memset_trap),
                                        .handler = user_memset_trap_handler});
}

PageMap* CreatePageMap() {
    PageMap* addressSpace = (PageMap*)kmalloc(sizeof(PageMap));

    pdpt_entry_t* pdpt = (pdpt_entry_t*)Memory::KernelAllocate4KPages(1); // PDPT;
    uintptr_t pdptPhys = Memory::AllocatePhysicalMemoryBlock();
    Memory::KernelMapVirtualMemory4K(pdptPhys, (uintptr_t)pdpt, 1);
    memset((pdpt_entry_t*)pdpt, 0, 4096);

    pd_entry_t** pageDirPointers = (pd_entry_t**)KernelAllocate4KPages(1); // Page Dirs
    Memory::KernelMapVirtualMemory4K(Memory::AllocatePhysicalMemoryBlock(), (uintptr_t)pageDirPointers, 1);
    uint64_t* pageDirPointersPhys = (uint64_t*)KernelAllocate4KPages(1); // Page Dirs
    Memory::KernelMapVirtualMemory4K(Memory::AllocatePhysicalMemoryBlock(), (uintptr_t)pageDirPointersPhys, 1);
    page_t*** pageTables = (page_t***)KernelAllocate4KPages(1); // Page Tables
    Memory::KernelMapVirtualMemory4K(Memory::AllocatePhysicalMemoryBlock(), (uintptr_t)pageTables, 1);

    pml4_entry_t* pml4 = (pml4_entry_t*)KernelAllocate4KPages(1); // Page Tables
    uintptr_t pml4Phys = Memory::AllocatePhysicalMemoryBlock();
    Memory::KernelMapVirtualMemory4K(pml4Phys, (uintptr_t)pml4, 1);
    memcpy(pml4, kernelPML4, 4096);

    pd_entry_t* pageDirs = (pd_entry_t*)KernelAllocate4KPages(1 * 512);

    for (int i = 0; i < 512; i++) {
        pageDirPointers[i] = pageDirs + 512 * i;
        pageDirPointersPhys[i] = Memory::AllocatePhysicalMemoryBlock();
        KernelMapVirtualMemory4K(pageDirPointersPhys[i], (uintptr_t)pageDirPointers[i], 1);

        pageTables[i] = (page_t**)kmalloc(4096);

        SetPageFrame(&(pdpt[i]), pageDirPointersPhys[i]);
        pdpt[i] |= PDPT_WRITABLE | PDPT_PRESENT | PDPT_USER;

        memset(pageDirPointers[i], 0, 4096);
        memset(pageTables[i], 0, 4096);
    }

    addressSpace->pageDirPointers = pageDirPointers;
    addressSpace->pageDirs = pageDirs;
    addressSpace->pageDirPointersPhys = pageDirPointersPhys;
    addressSpace->pageTables = pageTables;
    addressSpace->pml4 = pml4;
    addressSpace->pdptPhys = pdptPhys;
    addressSpace->pml4Phys = pml4Phys;
    addressSpace->pdpt = pdpt;

    pml4[0] = pdptPhys | PML4_PRESENT | PML4_WRITABLE | PAGE_USER;

    return addressSpace;
}

void DestroyPageMap(PageMap* pageMap) {
    for (int i = 0; i < DIRS_PER_PDPT; i++) {
        if (!pageMap->pageDirPointers[i]) {
            continue;
        }

        if (pageMap->pageDirPointersPhys[i] < PHYSALLOC_BLOCK_SIZE) {
            continue;
        }

        for (int j = 0; j < TABLES_PER_DIR; j++) {
            pd_entry_t dirEnt = pageMap->pageDirs[i * 512 + j];
            if (dirEnt & PAGE_PRESENT) {
                uint64_t phys = GetPageFrame(dirEnt);
                if (phys < PHYSALLOC_BLOCK_SIZE) {
                    continue;
                }

                FreePhysicalMemoryBlock(phys);
                KernelFree4KPages(pageMap->pageTables[i][j], 1);
            }
            pageMap->pageDirPointers[i][j] = 0;
        }

        if (pageMap->pageTables[i]) {
            kfree(pageMap->pageTables[i]);
        }

        pageMap->pdpt[i] = 0;
        Memory::FreePhysicalMemoryBlock(pageMap->pageDirPointersPhys[i]);

        pageMap->pageDirPointers[i] = 0;
    }

    KernelFree4KPages(pageMap->pageDirs, 512);
    Memory::FreePhysicalMemoryBlock(pageMap->pdptPhys);
}

bool CheckRegion(uintptr_t addr, uint64_t len, page_map_t* addressSpace) {
    return addr < PDPT_SIZE && (addr + len) < PDPT_SIZE && (addressSpace->pdpt[PDPT_GET_INDEX(addr)] & PDPT_USER) &&
           (addressSpace->pdpt[PDPT_GET_INDEX(addr + len)] & PDPT_USER);
}

bool CheckKernelPointer(uintptr_t addr, uint64_t len) {
    ScopedSpinLock<true> lock(kernelHeapDirLock);
    if (PML4_GET_INDEX(addr) != PML4_GET_INDEX(KERNEL_VIRTUAL_BASE)) {
        return 0;
    }

    if (!(kernelPDPT[PDPT_GET_INDEX(addr)] & 0x1)) {
        return 0;
    }

    if (PDPT_GET_INDEX(addr) == KERNEL_HEAP_PDPT_INDEX) {
        if (!(kernelHeapDir[PAGE_DIR_GET_INDEX(addr)] & 0x1)) {
            return 0;
        }

        if (!(kernelHeapDir[PAGE_DIR_GET_INDEX(addr)] & PDE_2M)) {
            if (!(kernelHeapDirTables[PAGE_DIR_GET_INDEX(addr)][PAGE_TABLE_GET_INDEX(addr)] & 0x1)) {
                return 0;
            }
        }
    } else if (PDPT_GET_INDEX(addr) == PDPT_GET_INDEX(KERNEL_VIRTUAL_BASE)) {
        if (!(kernelDir[PAGE_DIR_GET_INDEX(addr)] & 0x1)) {
            return 0;
        }
    } else {
        return 0;
    }

    return 1;
}

bool CheckUsermodePointer(uintptr_t addr, uint64_t len, AddressSpace* addressSpace) {
    return addressSpace->RangeInRegion(addr, len);
}

void* KernelAllocate4KPages(uint64_t amount) {
    ScopedSpinLock<true> lockKDir(kernelHeapDirLock);

    uint64_t offset = 0;
    uint64_t pageDirOffset = 0;
    uint64_t counter = 0;
    uintptr_t address = 0;

    uint64_t pml4Index = KERNEL_HEAP_PML4_INDEX;
    uint64_t pdptIndex = KERNEL_HEAP_PDPT_INDEX;

    /* Attempt 1: Already Allocated Page Tables*/
    for (int i = 0; i < TABLES_PER_DIR; i++) {
        if (kernelHeapDir[i] & 0x1 && !(kernelHeapDir[i] & 0x80)) {
            for (int j = 0; j < TABLES_PER_DIR; j++) {
                if (kernelHeapDirTables[i][j] & 0x1) {
                    pageDirOffset = i;
                    offset = j + 1;
                    counter = 0;
                    continue;
                }

                counter++;

                if (counter >= amount) {
                    address = (PDPT_SIZE * pml4Index) + (pdptIndex * PAGE_SIZE_1G) + (pageDirOffset * PAGE_SIZE_2M) +
                              (offset * PAGE_SIZE_4K);
                    address |= 0xFFFF000000000000;
                    while (counter--) {
                        if (offset >= 512) {
                            pageDirOffset++;
                            offset = 0;
                        }
                        kernelHeapDirTables[pageDirOffset][offset] = 0x3;
                        offset++;
                    }

                    return (void*)address;
                }
            }
        } else {
            pageDirOffset = i + 1;
            offset = 0;
            counter = 0;
        }
    }

    pageDirOffset = 0;
    offset = 0;
    counter = 0;

    /* Attempt 2: Allocate Page Tables*/
    for (int i = 0; i < TABLES_PER_DIR; i++) {
        if (!(kernelHeapDir[i] & 0x1)) {
            counter += 512;

            if (counter >= amount) {
                address = (PDPT_SIZE * pml4Index) + (pdptIndex * PAGE_SIZE_1G) + (pageDirOffset * PAGE_SIZE_2M) +
                          (offset * PAGE_SIZE_4K);
                address |= 0xFFFF000000000000;
                // kernelHeapDir[i] = (PAGE_FRAME & ((uintptr_t)&(kernelHeapDirTables[i]) - KERNEL_VIRTUAL_BASE)) | 0x3;
                SetPageFrame(&(kernelHeapDir[pageDirOffset]),
                             ((uintptr_t) & (kernelHeapDirTables[pageDirOffset]) - KERNEL_VIRTUAL_BASE));
                kernelHeapDir[pageDirOffset] |= 0x3;
                while (amount--) {
                    if (offset >= 512) {
                        pageDirOffset++;
                        offset = 0;
                        SetPageFrame(&(kernelHeapDir[pageDirOffset]),
                                     ((uintptr_t) & (kernelHeapDirTables[pageDirOffset]) - KERNEL_VIRTUAL_BASE));
                        kernelHeapDir[pageDirOffset] |= 0x3;
                    }
                    kernelHeapDirTables[pageDirOffset][offset] = 0x3;
                    offset++;
                }
                return (void*)address;
            }
        } else {
            pageDirOffset = i + 1;
            offset = 0;
            counter = 0;
        }
    }

    asm("cli");
    lockKDir.release();
    Log::DisableBuffer();
    Log::Info("Out of virtual memory, used physical memory: %d KB", Memory::usedPhysicalBlocks * 4);
    PrintStackTrace(GetRBP());
    KernelPanic("Out of Virtual Memory.");
}

void KernelFree4KPages(void* addr, uint64_t amount) {
    uint64_t pageDirIndex, pageIndex;
    uint64_t virt = (uint64_t)addr;

    ScopedSpinLock<true> lockKDir(kernelHeapDirLock);

    while (amount--) {
        pageDirIndex = PAGE_DIR_GET_INDEX(virt);
        pageIndex = PAGE_TABLE_GET_INDEX(virt);
        kernelHeapDirTables[pageDirIndex][pageIndex] = 0;
        invlpg(virt);
        virt += PAGE_SIZE_4K;
    }
}

void KernelMapVirtualMemory2M(uint64_t phys, uint64_t virt, uint64_t amount) {
    uint64_t pageDirIndex = PAGE_DIR_GET_INDEX(virt);

    ScopedSpinLock<true> lockKDir(kernelHeapDirLock);

    while (amount--) {
        kernelHeapDir[pageDirIndex] = 0x83;
        SetPageFrame(&(kernelHeapDir[pageDirIndex]), phys);
        kernelHeapDir[pageDirIndex] |= 0x83;
        pageDirIndex++;
        phys += PAGE_SIZE_2M;
    }
}

void KernelMapVirtualMemory4K(uint64_t phys, uint64_t virt, uint64_t amount, uint64_t flags) {
    uint64_t pageDirIndex, pageIndex;

    ScopedSpinLock<true> lockKDir(kernelHeapDirLock);

    while (amount--) {
        pageDirIndex = PAGE_DIR_GET_INDEX(virt);
        pageIndex = PAGE_TABLE_GET_INDEX(virt);
        kernelHeapDirTables[pageDirIndex][pageIndex] = flags;
        SetPageFrame(&(kernelHeapDirTables[pageDirIndex][pageIndex]), phys);
        invlpg(virt);
        phys += PAGE_SIZE_4K;
        virt += PAGE_SIZE_4K;
    }
}

void KernelMapVirtualMemory4K(uint64_t phys, uint64_t virt, uint64_t amount) {
    KernelMapVirtualMemory4K(phys, virt, amount, PAGE_WRITABLE | PAGE_PRESENT);
}

void MapVirtualMemory4K(uint64_t phys, uint64_t virt, uint64_t amount, uint64_t flags, PageMap* pageMap) {
    uint64_t pml4Index, pdptIndex, pageDirIndex, pageIndex;

    while (amount--) {
        pml4Index = PML4_GET_INDEX(virt);
        pdptIndex = PDPT_GET_INDEX(virt);
        pageDirIndex = PAGE_DIR_GET_INDEX(virt);
        pageIndex = PAGE_TABLE_GET_INDEX(virt);

        const char* panic[1] = {"Process address space cannot be >512GB"};
        if (pdptIndex > MAX_PDPT_INDEX || pml4Index)
            KernelPanic(panic, 1);

        assert(pageMap->pageDirPointers[pdptIndex]);
        if (!(pageMap->pageDirs[pdptIndex * 512 + pageDirIndex] & 0x1))
            CreatePageTable(pdptIndex, pageDirIndex,
                            pageMap); // If we don't have a page table at this address, create one.

        assert(pageMap->pageTables[pdptIndex][pageDirIndex]);
        pageMap->pageTables[pdptIndex][pageDirIndex][pageIndex] = (phys & PAGE_FRAME) | flags;

        invlpg(virt);

        phys += PAGE_SIZE_4K;
        virt += PAGE_SIZE_4K; /* Go to next page */
    }
}

void MapVirtualMemory4K(uint64_t phys, uint64_t virt, uint64_t amount, PageMap* pageMap) {
    MapVirtualMemory4K(phys, virt, amount, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER, pageMap);
}

uintptr_t GetIOMapping(uintptr_t addr) {
    if (addr > 0xffffffff) { // Typically most MMIO will not reside > 4GB, but check just in case
        Log::Error("MMIO >4GB current unsupported");
        return 0xffffffff;
    }

    return addr + IO_VIRTUAL_BASE;
}

void RegisterPageFaultTrap(PageFaultTrap trap) { pageFaultTraps->insert(trap.instructionPointer, trap); }

void GPFHandler(void*, RegisterContext* regs) {
    if(!(regs->ss & 0x3)) {
        // Fault occurred in kernel
        if (PageFaultTrap trap; pageFaultTraps->get(regs->rip, trap)) {
            // If we have found a handler, set the IP to the handler
            // and run
            regs->rip = reinterpret_cast<uintptr_t>(trap.handler);
            return;
        }
    }

    IDT::HandleFatalInterrupt(0xd, regs);
}

void PageFaultHandler(void*, RegisterContext* regs) {
    uint64_t faultAddress;
    asm volatile("movq %%cr2, %0" : "=r"(faultAddress));

    int errorCode = regs->err;
    int present = !(errorCode & 0x1); // Page not present
    int rw = errorCode & 0x2;         // Attempted write to read only page
    int us = errorCode & 0x4;         // Processor was in user-mode and tried to access kernel page
    int reserved = errorCode & 0x8;   // Overwritten CPU-reserved bits of page entry
    int id = errorCode & 0x10;        // Caused by an instruction fetch

    Process* process = Process::current();
    Thread* thread = Thread::current();

    // We only want to dump fault information when it is fatal
    auto dumpFaultInformation = [&]() -> void {
        Log::DisableBuffer();
        Log::SetVideoConsole(nullptr);

        Log::Info("Page Fault");

        Log::Info("Register Dump:\nrip:%x, rax: %x, rbx: %x, rcx: %x, rdx: %x, rsi: %x, rdi: %x, rsp: %x, rbp: %x",
                  regs->rip, regs->rax, regs->rbx, regs->rcx, regs->rdx, regs->rsi, regs->rdi, regs->rsp, regs->rbp);

        Log::Info("Fault address: %x", faultAddress);

        if (present)
            Log::Info("Page not present"); // Print fault to serial
        if (rw)
            Log::Info("Read Only");
        if (us)
            Log::Info("User mode process tried to access kernel memory");
        if (reserved)
            Log::Info("Reserved");
        if (id)
            Log::Info("instruction fetch");

        if (process) {
            Log::Info("Process Mapped Memory:");
            process->addressSpace->DumpRegions();
        }
    };

    if ((regs->cs & 0x3)) {  
        // If the thread is zombified, do not bother acquiring the lock
        if(thread->state == ThreadStateZombie) {
            Log::Info("Zombie thread page fault");
            asm("sti");
            for(;;) {
                Scheduler::Yield();
            }
        } 
        
        // Make sure we acquired the lock
        int res = acquireTestLock(&Thread::current()->kernelLock); // Prevent the thread from being killed, etc.
        if (res) {
            Log::Info("Process %s (PID: %x) page fault.", process->name, process->pid());
            dumpFaultInformation();

            Log::Info("Stack trace:");
            UserPrintStackTrace(regs->rbp, Scheduler::GetCurrentProcess()->addressSpace);
            Log::Info("End stack trace.");

            assert(!res);
        }
    }

    if (process) {
        AddressSpace* addressSpace = process->addressSpace;
        asm("sti");
        MappedRegion* faultRegion =
            addressSpace->AddressToRegionWriteLock(faultAddress); // Remember that this acquires a lock
        asm("cli");
        if (faultRegion &&
            faultRegion->vmObject.get()) { // If there is a corresponding VMO for the fault then this is not an error
            FancyRefPtr<VMObject> vmo = faultRegion->vmObject;
            if (vmo->IsCopyOnWrite() && rw /* Attempted to write to read-only page */) {
                if (vmo->refCount <= 1) { // Last reference, no need to clone
                    vmo->copyOnWrite = false;
                    vmo->MapAllocatedBlocks(
                        faultRegion->Base(),
                        addressSpace->get_page_map()); // This should remap all allocated blocks as writable

                    asm("sti");
                    vmo->Hit(faultRegion->Base(), faultAddress - faultRegion->Base(),
                             addressSpace->get_page_map()); // In case the block was never allocated in the first place
                    asm("cli");

                    faultRegion->lock.release_write();
                    if ((regs->cs & 0x3)) {
                        releaseLock(&Thread::current()->kernelLock);
                    }
                    return;
                } else {
                    asm("sti");
                    VMObject* clone = vmo->Clone();

                    vmo->refCount--;
                    faultRegion->vmObject = clone;
                    asm("cli");

                    clone->MapAllocatedBlocks(faultRegion->Base(), addressSpace->get_page_map());
                    clone->Hit(faultRegion->Base(), faultAddress - faultRegion->Base(),
                               addressSpace->get_page_map()); // In case the block was never allocated in the first place

                    faultRegion->lock.release_write();

                    if ((regs->cs & 0x3)) {
                        releaseLock(&Thread::current()->kernelLock);
                    }
                    return;
                }
            }

            asm("sti");
            int status = faultRegion->vmObject->Hit(faultRegion->Base(), faultAddress - faultRegion->Base(),
                                                    addressSpace->get_page_map());
            faultRegion->lock.release_write();

            if (!status) {
                if ((regs->cs & 0x3)) {
                    releaseLock(&Thread::current()->kernelLock);
                }
                return; // Success!
            }
            asm("cli");
        } else if (faultRegion) {
            faultRegion->lock.release_write();
        } else if (PageFaultTrap trap; pageFaultTraps->get(regs->rip, trap)) {
            // If we have found a handler, set the IP to the handler
            // and run
            regs->rip = reinterpret_cast<uintptr_t>(trap.handler);
            return;
        }
    }

    if ((regs->cs & 0x3)) {
        assert(process);

        Log::Info("Process %s (PID: %x) page fault.", process->name, process->pid());
        dumpFaultInformation();

        Log::Info("Stack trace:");
        UserPrintStackTrace(regs->rbp, Scheduler::GetCurrentProcess()->addressSpace);
        Log::Info("End stack trace.");

        Process::current()->die();
        return;
    }

    asm("cli");
    dumpFaultInformation();

    // Kernel Panic so tell other processors to stop executing
    APIC::Local::SendIPI(0, ICR_DSH_OTHER /* Send to all other processors except us */, ICR_MESSAGE_TYPE_FIXED,
                         IPI_HALT);

    Log::Info("Stack trace:");
    PrintStackTrace(regs->rbp);
    Log::Info("End stack trace.");

    char temp[19];
    itoa(regs->rip, temp, 16);
    temp[18] = 0;

    char temp2[19];
    itoa(faultAddress, temp2, 16);
    temp2[18] = 0;

    const char* reasons[]{"Page Fault", "RIP: ", temp, "Address: ", temp2};
    KernelPanic(reasons, 5);
    for (;;)
        ;
}
} // namespace Memory
