/// @file paging.c
/// @brief Implementation of a memory paging management.
/// @copyright (c) 2014-2024 This file is distributed under the MIT License.
/// See LICENSE.md for details.

// Setup the logging for this file (do this before any other include).
#include "sys/kernel_levels.h"           // Include kernel log levels.
#define __DEBUG_HEADER__ "[PAGING]"      ///< Change header.
#define __DEBUG_LEVEL__  LOGLEVEL_NOTICE ///< Set log level.
#include "io/debug.h"                    // Include debugging functions.

#include "assert.h"
#include "descriptor_tables/isr.h"
#include "fs/vfs.h"
#include "list_head.h"
#include "list_head_algorithm.h"
#include "mem/alloc/zone_allocator.h"
#include "mem/paging.h"
#include "mem/mm/vmem.h"
#include "process/scheduler.h"
#include "stddef.h"
#include "stdint.h"
#include "string.h"
#include "sys/mman.h"
#include "system/panic.h"

/// Cache for storing page directories.
kmem_cache_t *pgdir_cache;
/// Cache for storing page tables.
kmem_cache_t *pgtbl_cache;

/// @brief Structure for iterating page directory entries.
typedef struct page_iterator_s {
    /// Pointer to the entry.
    page_dir_entry_t *entry;
    /// Pointer to the page table.
    page_table_t *table;
    /// Page Frame Number (PFN).
    uint32_t pfn;
    /// Last PNF.
    uint32_t last_pfn;
    /// Contains MEMMAP_FLAGS flags.
    uint32_t flags;
} page_iterator_t;

/// @brief Structure for iterating page table entries.
typedef struct pg_iter_entry_s {
    /// Pointer to the page table entry.
    page_table_entry_t *entry;
    /// Page Frame Number (PFN).
    uint32_t pfn;
} pg_iter_entry_t;

page_directory_t *paging_get_main_directory(void)
{
    // Ensure the main_mm structure is initialized.
    if (!mm_get_main()) {
        pr_crit("main_mm is not initialized\n");
        return NULL;
    }

    // Return the pointer to the main page directory.
    return mm_get_main()->pgd;
}

int is_current_pgd(page_directory_t *pgd)
{
    // Check if the pgd pointer is NULL
    if (pgd == NULL) {
        return 0;
    }
    // Compare the given pgd with the current page directory
    return pgd == paging_get_current_directory();
}

int paging_switch_directory_va(page_directory_t *dir)
{
    // Ensure the directory pointer is valid.
    if (!dir) {
        pr_crit("Invalid page directory pointer\n");
        return -1;
    }

    // Get the low memory page corresponding to the given directory address.
    page_t *page = get_page_from_virtual_address((uintptr_t)dir);
    if (!page) {
        pr_crit("Failed to get low memory page from address\n");
        return -1;
    }

    // Get the physical address of the low memory page.
    uintptr_t phys_addr = get_physical_address_from_page(page);
    if (!phys_addr) {
        pr_crit("Failed to get physical address from page\n");
        return -1;
    }

    // Switch to the new paging directory using the physical address.
    paging_switch_directory((page_directory_t *)phys_addr);

    return 0;
}

void paging_flush_tlb_single(unsigned long addr) { __asm__ __volatile__("invlpg (%0)" ::"r"(addr) : "memory"); }

/// @brief Initializes the page directory.
/// @param pdir the page directory to initialize.
static void __init_pagedir(page_directory_t *pdir) { *pdir = (page_directory_t){{0}}; }

/// @brief Initializes the page table.
/// @param ptable the page table to initialize.
static void __init_pagetable(page_table_t *ptable) { *ptable = (page_table_t){{0}}; }

int paging_init(boot_info_t *info)
{
    // Check if the info pointer is valid.
    if (!info) {
        pr_crit("Invalid boot info provided.\n");
        return -1;
    }

    if (mm_init() < 0) {
        pr_crit("Failed to initialize memory management.\n");
        return -1;
    }

    if (vm_area_init() < 0) {
        pr_crit("Failed to initialize vm_area.\n");
        return -1;
    }

    // Create cache for page directory with custom constructor function.
    pgdir_cache = KMEM_CREATE_CTOR(page_directory_t, __init_pagedir);
    if (!pgdir_cache) {
        pr_crit("Failed to create pgdir_cache.\n");
        return -1;
    }

    // Create cache for page table with custom constructor function.
    pgtbl_cache = KMEM_CREATE_CTOR(page_table_t, __init_pagetable);
    if (!pgtbl_cache) {
        pr_crit("Failed to create pgtbl_cache.\n");
        return -1;
    }

    // Get the main memory management structure.
    mm_struct_t *main_mm = mm_get_main();

    // Allocate the page directory for the main memory management structure.
    main_mm->pgd = kmem_cache_alloc(pgdir_cache, GFP_KERNEL);
    if (!main_mm->pgd) {
        pr_crit("Failed to allocate main_mm page directory.\n");
        return -1;
    }

    // Calculate the size of low kernel memory.
    uint32_t lowkmem_size = info->stack_end - info->kernel_start;

    // Map the first 1MB of memory with physical mapping to access video memory and other BIOS functions.
    if (mem_upd_vm_area(main_mm->pgd, 0, 0, 1024 * 1024, MM_RW | MM_PRESENT | MM_GLOBAL | MM_UPDADDR) < 0) {
        pr_crit("Failed to map the first 1MB of memory.\n");
        return -1;
    }

    // Map the kernel memory region into the virtual memory space.
    if (mem_upd_vm_area(
            main_mm->pgd, info->kernel_start, info->kernel_phy_start, lowkmem_size,
            MM_RW | MM_PRESENT | MM_GLOBAL | MM_UPDADDR) < 0) {
        pr_crit("Failed to map kernel memory region.\n");
        return -1;
    }

    // Install the page fault interrupt service routine (ISR) handler.
    if (isr_install_handler(PAGE_FAULT, page_fault_handler, "page_fault_handler") < 0) {
        pr_crit("Failed to install page fault handler.\n");
        return -1;
    }

    // Switch to the newly created page directory.
    paging_switch_directory_va(main_mm->pgd);

    // Enable paging.
    paging_enable();

    return 0;
}

// Error code interpretation.
#define ERR_PRESENT  0x01 ///< Page not present.
#define ERR_RW       0x02 ///< Page is read only.
#define ERR_USER     0x04 ///< Page is privileged.
#define ERR_RESERVED 0x08 ///< Overwrote reserved bit.
#define ERR_INST     0x10 ///< Instruction fetch.

/// @brief Sets the given page table flags.
/// @param table the page table.
/// @param flags the flags to set.
static inline void __set_pg_table_flags(page_table_entry_t *table, uint32_t flags)
{
    // Check if the table pointer is valid.
    if (!table) {
        pr_crit("Invalid page table entry provided.\n");
        return; // Exit the function early if the table is null.
    }
    // Set the Read/Write flag: 1 if the MM_RW flag is set, 0 otherwise.
    table->rw         = (flags & MM_RW) != 0;
    // Set the Present flag: 1 if the MM_PRESENT flag is set, 0 otherwise.
    table->present    = (flags & MM_PRESENT) != 0;
    // Set the Copy-On-Write flag: 1 if the MM_COW flag is set, 0 otherwise.
    // This flag is used to track if the page is a copy-on-write page.
    table->kernel_cow = (flags & MM_COW) != 0;
    // Set the Available bits: these are reserved for future use, so set them to 1.
    table->available  = 1; // Currently just sets this to 1 as a placeholder.
    // Set the Global flag: 1 if the MM_GLOBAL flag is set, 0 otherwise.
    table->global     = (flags & MM_GLOBAL) != 0;
    // Set the User flag: 1 if the MM_USER flag is set, 0 otherwise.
    table->user       = (flags & MM_USER) != 0;
}

/// @brief Prints stack frame data and calls kernel_panic.
/// @param f    The interrupt stack frame.
/// @param addr The faulting address.
static void __page_fault_panic(pt_regs *f, uint32_t addr)
{
    __asm__ __volatile__("cli");

    // Gather fault info and print to screen
    pr_err("Faulting address (cr2): 0x%p\n", addr);

    pr_err("EIP: 0x%p\n", f->eip);

    pr_err("Page fault: 0x%x\n", addr);

    pr_err("Possible causes: [ ");
    if (!(f->err_code & ERR_PRESENT)) {
        pr_err("Page not present ");
    }
    if (f->err_code & ERR_RW) {
        pr_err("Page is read only ");
    }
    if (f->err_code & ERR_USER) {
        pr_err("Page is privileged ");
    }
    if (f->err_code & ERR_RESERVED) {
        pr_err("Overwrote reserved bits ");
    }
    if (f->err_code & ERR_INST) {
        pr_err("Instruction fetch ");
    }
    pr_err("]\n");
    PRINT_REGS(pr_err, f);

    kernel_panic("Page fault!");

    // Make directory accessible
    //    main_mm->pgd->entries[addr/(1024*4096)].user = 1;
    //    main_directory->entries[addr/(1024*4096)]. = 1;

    __asm__ __volatile__("cli");
}

/// @brief Handles the Copy-On-Write (COW) mechanism for a page table entry.
///        If the page is marked as COW, it allocates a new page and updates the entry.
/// @param entry The page table entry to manage.
/// @return 0 on success, 1 on error.
static int __page_handle_cow(page_table_entry_t *entry)
{
    // Check if the entry pointer is valid.
    if (!entry) {
        pr_crit("Invalid page table entry provided.\n");
        return 1;
    }

    // Check if the page is Copy On Write (COW).
    if (entry->kernel_cow) {
        // Mark the page as no longer Copy-On-Write.
        entry->kernel_cow = 0;

        // If the page is not currently present (not allocated in physical memory).
        if (!entry->present) {
            // Allocate a new physical page using high user memory flag.
            page_t *page = alloc_pages(GFP_HIGHUSER, 0);
            if (!page) {
                pr_crit("Failed to allocate a new page.\n");
                return 1;
            }

            // Map the allocated physical page to a virtual address.
            uint32_t vaddr = vmem_map_physical_pages(page, 1);
            if (!vaddr) {
                pr_crit("Failed to map the physical page to virtual address.\n");
                return 1;
            }

            // Clear the new page by setting all its bytes to 0.
            memset((void *)vaddr, 0, PAGE_SIZE);

            // Unmap the virtual address after clearing the page.
            vmem_unmap_virtual_address(vaddr);

            // Set the physical frame address of the allocated page into the entry.
            entry->frame = get_physical_address_from_page(page) >> 12U; // Shift to get page frame number.

            // Mark the page as present in memory.
            entry->present = 1;

            // Success, COW handled and page allocated.
            return 0;
        }
    }

    // If the page is not marked as COW, print an error.
    pr_err("Page not marked as copy-on-write (COW)!\n");

    // Return error as the page is not COW.
    return 1;
}

/// @brief Allocates memory for a page table entry.
/// @details If the page table is not present, allocates a new one and sets
/// flags accordingly.
/// @param entry The page directory entry for which memory is being allocated.
/// @param flags The flags that control the allocation, such as permissions and
/// attributes.
/// @return A pointer to the allocated page table, or NULL if allocation fails.
static page_table_t *__mem_pg_entry_alloc(page_dir_entry_t *entry, uint32_t flags)
{
    // Check if the page directory entry is valid.
    if (!entry) {
        pr_crit("Invalid page directory entry provided.\n");
        return NULL;
    }

    // If the page table is not present, allocate a new one.
    if (!entry->present) {
        // Mark the page table as present and set read/write and global/user flags.
        entry->present   = 1;                        // Indicate that the page table has been allocated.
        entry->rw        = 1;                        // Allow read/write access by default.
        entry->global    = (flags & MM_GLOBAL) != 0; // Set global flag if specified.
        entry->user      = (flags & MM_USER) != 0;   // Set user-mode flag if specified.
        entry->accessed  = 0;                        // Mark as not accessed.
        entry->available = 1;                        // Available for kernel use.

        // Allocate the page table using a memory cache.
        page_table_t *new_table = kmem_cache_alloc(pgtbl_cache, GFP_KERNEL);
        if (!new_table) {
            pr_crit("Failed to allocate memory for page table.\n");
            return NULL;
        }

        // Return the newly allocated page table.
        return new_table;
    }

    // If the page table is already present, update the flags accordingly.
    entry->present |= (flags & MM_PRESENT) != 0; // Update the present flag if MM_PRESENT is set.
    entry->rw |= (flags & MM_RW) != 0;           // Update the read/write flag if MM_RW is set.

    // Ensure that the global flag is not removed if it was previously set.
    // Removing a global flag from a page directory might indicate a bug in the kernel.
    if (entry->global && !(flags & MM_GLOBAL)) {
        kernel_panic("Attempted to remove the global flag from a page "
                     "directory entry.\n");
    }

    // Update the global and user flags.
    entry->global &= (flags & MM_GLOBAL) != 0; // Keep the global flag if specified.
    entry->user |= (flags & MM_USER) != 0;     // Set the user-mode flag if specified.

    // Retrieve the physical address of the page.
    page_t *page = get_page_from_physical_address(((uint32_t)entry->frame) << 12U);
    if (!page) {
        pr_crit("Failed to retrieve page from physical address.\n");
        return NULL;
    }

    // Convert the physical address into a low memory address.
    page_table_t *lowmem_addr = (page_table_t *)get_virtual_address_from_page(page);
    if (!lowmem_addr) {
        pr_crit("Failed to map page to low memory address.\n");
        return NULL;
    }

    // Return the mapped page table.
    return lowmem_addr;
}

/// @brief Sets the frame attribute of a page directory entry based on the page table's physical address.
/// @param entry The page directory entry to modify.
/// @param table The page table whose frame is being set in the directory entry.
/// @return 0 on success, -1 on failure.
static inline int __set_pg_entry_frame(page_dir_entry_t *entry, page_table_t *table)
{
    // Ensure the entry is not NULL.
    if (!entry) {
        pr_crit("Invalid page directory entry provided.\n");
        return -1;
    }

    // Ensure the table is not NULL.
    if (!table) {
        pr_crit("Invalid page table provided.\n");
        return -1;
    }

    // Retrieve the low memory page structure from the virtual address of the table.
    page_t *table_page = get_page_from_virtual_address((uint32_t)table);
    if (!table_page) {
        pr_crit("Failed to retrieve low memory page from table address: %p\n", table);
        return -1;
    }

    // Retrieve the physical address from the page structure.
    uint32_t phy_addr = get_physical_address_from_page(table_page);
    if (!phy_addr) {
        pr_crit("Failed to retrieve physical address from page: %p\n", table_page);
        return -1;
    }

    // Set the frame attribute in the page directory entry (shifted by 12 bits
    // to represent the frame number).
    entry->frame = phy_addr >> 12U;

    return 0;
}

void page_fault_handler(pt_regs *f)
{
    // Here you will find the `Demand Paging` mechanism.
    // From `Understanding The Linux Kernel 3rd Edition`: The term demand paging denotes a dynamic memory allocation
    // technique that consists of deferring page frame allocation until the last possible moment—until the process
    // attempts to address a page that is not present in RAM, thus causing a Page Fault exception.
    //
    // So, if you go inside `mentos/src/exceptions.S`, and check out the macro ISR_ERR, we are pushing the error code on
    // the stack before firing a page fault exception. The error code must be analyzed by the exception handler to
    // determine how to handle the exception. The following bits are the only ones used, all others are reserved.
    // | US RW  P | Description
    // |  0  0  0 | Supervisory process tried to read a non-present page entry
    // |  0  0  1 | Supervisory process tried to read a page and caused a protection fault
    // |  0  1  0 | Supervisory process tried to write to a non-present page entry
    // |  0  1  1 | Supervisory process tried to write a page and caused a protection fault
    // |  1  0  0 | User process tried to read a non-present page entry
    // |  1  0  1 | User process tried to read a page and caused a protection fault
    // |  1  1  0 | User process tried to write to a non-present page entry
    // |  1  1  1 | User process tried to write a page and caused a protection fault

    // Extract the error
    int err_user    = bit_check(f->err_code, 2) != 0;
    int err_rw      = bit_check(f->err_code, 1) != 0;
    int err_present = bit_check(f->err_code, 0) != 0;

    // Extract the address that caused the page fault from the CR2 register.
    uint32_t faulting_addr = get_cr2();

    // Retrieve the current page directory's physical address.
    uint32_t phy_dir = (uint32_t)paging_get_current_directory();
    if (!phy_dir) {
        pr_crit("Failed to retrieve current page directory.\n");
        __page_fault_panic(f, faulting_addr);
    }

    // Get the page from the physical address of the directory.
    page_t *dir_page = get_page_from_physical_address(phy_dir);
    if (!dir_page) {
        pr_crit("Failed to get page from physical address: %p\n", (void *)phy_dir);
        __page_fault_panic(f, faulting_addr);
    }

    // Get the low memory address from the page and cast it to a page directory structure.
    page_directory_t *lowmem_dir = (page_directory_t *)get_virtual_address_from_page(dir_page);
    if (!lowmem_dir) {
        pr_crit("Failed to get low memory address from page: %p\n", (void *)dir_page);
        __page_fault_panic(f, faulting_addr);
    }

    // Get the directory entry that corresponds to the faulting address.
    page_dir_entry_t *direntry = &lowmem_dir->entries[faulting_addr / (1024U * PAGE_SIZE)];

    // Panic only if page is in kernel memory, else abort process with SIGSEGV.
    if (!direntry->present) {
        pr_crit("ERR(0): Page directory entry not present (%d%d%d)\n", err_user, err_rw, err_present);

        // If the fault was caused by a user process, send a SIGSEGV signal.
        if (err_user) {
            task_struct *task = scheduler_get_current_process();
            if (task) {
                // Notifies current process.
                sys_kill(task->pid, SIGSEGV);
                // Now, we know the process needs to be removed from the list of
                // running processes. We pushed the SEGV signal in the queues of
                // signal to send to the process. To properly handle the signal,
                // just run scheduler.
                scheduler_run(f);
                return;
            }
        }
        pr_crit("ERR(0): So, it is not present, and it was not the user.\n");
        __page_fault_panic(f, faulting_addr);
    }

    // Retrieve the physical address of the page table.
    uint32_t phy_table = direntry->frame << 12U;

    // Get the page from the physical address of the page table.
    page_t *table_page = get_page_from_physical_address(phy_table);
    if (!table_page) {
        pr_crit("Failed to get page from physical address: %p\n", (void *)phy_table);
        __page_fault_panic(f, faulting_addr);
    }

    // Get the low memory address from the page and cast it to a page table structure.
    page_table_t *lowmem_table = (page_table_t *)get_virtual_address_from_page(table_page);
    if (!lowmem_table) {
        pr_crit("Failed to get low memory address from page: %p\n", (void *)table_page);
        __page_fault_panic(f, faulting_addr);
    }

    // Get the entry inside the table that caused the fault.
    uint32_t table_index = (faulting_addr / PAGE_SIZE) % 1024U;

    // Get the corresponding page table entry.
    page_table_entry_t *entry = &lowmem_table->pages[table_index];
    if (!entry) {
        pr_crit("Failed to retrieve page table entry.\n");
        __page_fault_panic(f, faulting_addr);
    }

    // There was a page fault on a virtual mapped address, so we must first
    // update the original mapped page
    if (is_valid_virtual_address(faulting_addr)) {
        // Get the original page table entry from the virtually mapped one.
        page_table_entry_t *orig_entry = (page_table_entry_t *)(*(uint32_t *)entry);
        if (!orig_entry) {
            pr_crit("Original page table entry is NULL.\n");
            __page_fault_panic(f, faulting_addr);
        }

        // Check if the page is Copy on Write (CoW).
        if (__page_handle_cow(orig_entry)) {
            pr_crit("ERR(1): %d%d%d\n", err_user, err_rw, err_present);
            __page_fault_panic(f, faulting_addr);
        }

        // Update the page table entry frame.
        entry->frame = orig_entry->frame;

        // Update the entry flags.
        __set_pg_table_flags(entry, MM_PRESENT | MM_RW | MM_GLOBAL | MM_COW | MM_UPDADDR);
    } else {
        // Check if the page is Copy on Write (CoW).
        if (__page_handle_cow(entry)) {
            pr_crit(
                "Page fault caused by Copy on Write (CoW). Flags: user=%d, "
                "rw=%d, present=%d\n",
                err_user, err_rw, err_present);
            // If the fault was caused by a user process, send a SIGSEGV signal.
            if (err_user && err_rw && err_present) {
                // Get the current process.
                task_struct *task = scheduler_get_current_process();
                if (task) {
                    // Notifies current process.
                    sys_kill(task->pid, SIGSEGV);
                    // Now, we know the process needs to be removed from the list of
                    // running processes. We pushed the SEGV signal in the queues of
                    // signal to send to the process. To properly handle the signal,
                    // just run scheduler.
                    scheduler_run(f);
                    return;
                }
                pr_crit("No task found for current process, unable to send "
                        "SIGSEGV.\n");
            } else {
                pr_crit("Invalid flags for CoW handling, continuing...\n");
            }
            pr_crit("Continuing with page fault handling, triggering panic.\n");
            __page_fault_panic(f, faulting_addr);
        }
    }

    // Invalidate the TLB entry for the faulting address.
    paging_flush_tlb_single(faulting_addr);
}

/// @brief Initialize a page iterator.
/// @param iter       The iterator to initialize.
/// @param pgd        The page directory to iterate.
/// @param addr_start The starting address.
/// @param size       The total amount we want to iterate.
/// @param flags      Allocation flags.
/// @return 0 on success, -1 on error.
static int
__pg_iter_init(page_iterator_t *iter, page_directory_t *pgd, uint32_t addr_start, uint32_t size, uint32_t flags)
{
    // Calculate the starting page frame number (PFN) based on the starting address.
    uint32_t start_pfn = addr_start / PAGE_SIZE;

    // Calculate the ending page frame number (PFN) based on the starting address and size.
    uint32_t end_pfn = (addr_start + size + PAGE_SIZE - 1) / PAGE_SIZE;

    // Determine the base page table index from the starting PFN.
    uint32_t base_pgt = start_pfn / 1024;

    // Ensure that the base page table index is within valid range.
    if (base_pgt >= MAX_PAGE_TABLE_ENTRIES) {
        pr_crit("Base page table index %u is out of bounds.\n", base_pgt);
        return -1;
    }

    // Initialize the iterator's entry pointer to point to the corresponding page directory entry.
    iter->entry = pgd->entries + base_pgt;

    // Set the page frame numbers for the iterator.
    iter->pfn      = start_pfn;
    iter->last_pfn = end_pfn;
    iter->flags    = flags;

    // Allocate memory for the page table entry associated with the iterator.
    iter->table = __mem_pg_entry_alloc(iter->entry, flags);
    // Check if the allocation was successful.
    if (!iter->table) {
        pr_crit("Failed to allocate memory for page table entry.\n");
        return -1;
    }

    // Set the frame for the page entry.
    __set_pg_entry_frame(iter->entry, iter->table);

    return 0;
}

/// @brief Checks if the iterator has a next entry.
/// @param iter The iterator to check.
/// @return Returns 1 if the iterator can continue the iteration; otherwise, returns 0.
static int __pg_iter_has_next(page_iterator_t *iter)
{
    // Check for a null iterator pointer to avoid dereferencing a null pointer.
    if (!iter) {
        pr_crit("The page iterator is null.\n");
        return 0;
    }

    // Check if the current page frame number (pfn) is less than the last page frame number (last_pfn).
    // This condition determines if there are more entries to iterate over.
    return iter->pfn < iter->last_pfn;
}

/// @brief Moves the iterator to the next entry.
/// @param iter The iterator to advance.
/// @return The current entry after moving to the next entry.
static pg_iter_entry_t __pg_iter_next(page_iterator_t *iter)
{
    // Check for a null iterator pointer to avoid dereferencing a null pointer.
    if (!iter) {
        pr_crit("The page iterator is null.\n");
        return (pg_iter_entry_t){0};
    }

    // Initialize the result entry with the current page frame number (pfn).
    pg_iter_entry_t result = {.entry = &iter->table->pages[iter->pfn % 1024], .pfn = iter->pfn};

    // Move to the next page frame number.
    iter->pfn++;

    // Check if we have wrapped around to a new page.
    if (iter->pfn % 1024 == 0) {
        // Check if we haven't reached the end of the last page.
        if (iter->pfn != iter->last_pfn) {
            // Ensure that the new entry address is valid and page-aligned.
            if (((uint32_t)++iter->entry) % 4096 != 0) {
                // Attempt to allocate memory for a new page entry.
                iter->table = __mem_pg_entry_alloc(iter->entry, iter->flags);
                if (!iter->table) {
                    pr_crit("Failed to allocate memory for new page entry.\n");
                    return (pg_iter_entry_t){0};
                }

                // Set the frame for the newly allocated entry.
                __set_pg_entry_frame(iter->entry, iter->table);
            }
        }
    }

    // Return the current entry after moving to the next.
    return result;
}

page_t *mem_virtual_to_page(page_directory_t *pgd, uint32_t virt_start, size_t *size)
{
    // Check for null pointer to the page directory to avoid dereferencing.
    if (!pgd) {
        pr_crit("The page directory is null.\n");
        return NULL;
    }

    // Calculate the page frame number and page table index from the virtual address.
    uint32_t virt_pfn        = virt_start / PAGE_SIZE;
    uint32_t virt_pgt        = virt_pfn / 1024; // Page table index.
    uint32_t virt_pgt_offset = virt_pfn % 1024; // Offset within the page table.

    // Get the physical page for the page directory entry.
    page_t *pgd_page = memory.mem_map + pgd->entries[virt_pgt].frame;

    // Get the low memory address of the page table.
    page_table_t *pgt_address = (page_table_t *)get_virtual_address_from_page(pgd_page);
    if (!pgt_address) {
        pr_crit("Failed to get low memory address from page directory entry.\n");
        return NULL;
    }

    // Get the physical frame number for the corresponding entry in the page table.
    uint32_t pfn = pgt_address->pages[virt_pgt_offset].frame;

    // Map the physical frame number to a physical page.
    page_t *page = memory.mem_map + pfn;

    // FIXME: handle unaligned page mapping to return the correct to-block-end
    // size instead of returning 0 (1 page at a time).
    if (size) {
        uint32_t pfn_count   = 1U << page->bbpage.order; // Calculate the number of pages.
        uint32_t bytes_count = pfn_count * PAGE_SIZE;    // Calculate the total byte count.
        *size                = min(*size,
                                   bytes_count); // Store the size, ensuring it doesn't exceed the maximum.
    }

    // Return the pointer to the mapped physical page.
    return page;
}

int mem_upd_vm_area(page_directory_t *pgd, uint32_t virt_start, uint32_t phy_start, size_t size, uint32_t flags)
{
    // Check for null pointer to the page directory to avoid dereferencing.
    if (!pgd) {
        pr_crit("The page directory is null.\n");
        return -1;
    }

    // Initialize the page iterator for the virtual memory area.
    page_iterator_t virt_iter;
    if (__pg_iter_init(&virt_iter, pgd, virt_start, size, flags) < 0) {
        pr_crit("Failed to initialize source page iterator\n");
        return -1;
    }

    // Calculate the starting page frame number for the physical address.
    uint32_t phy_pfn = phy_start / PAGE_SIZE;

    // Iterate through the virtual memory area.
    while (__pg_iter_has_next(&virt_iter)) {
        pg_iter_entry_t it = __pg_iter_next(&virt_iter);

        // If the MM_UPDADDR flag is set, update the frame address.
        if (flags & MM_UPDADDR) {
            // Ensure the physical frame number is valid before assignment.
            if (phy_pfn >= MAX_PHY_PFN) {
                pr_crit("Physical frame number exceeds maximum limit.\n");
                return -1;
            }
            it.entry->frame = phy_pfn++;
            // Flush the TLB only if the page directory is the current one.
            paging_flush_tlb_single(it.pfn * PAGE_SIZE);
        }

        // Set the page table flags.
        __set_pg_table_flags(it.entry, flags);
    }

    return 0;
}

int mem_clone_vm_area(
    page_directory_t *src_pgd,
    page_directory_t *dst_pgd,
    uint32_t src_start,
    uint32_t dst_start,
    size_t size,
    uint32_t flags)
{
    // Check for null pointer.
    if (!src_pgd) {
        pr_crit("The source page directory is null.\n");
        return -1;
    }

    // Check for null pointer.
    if (!dst_pgd) {
        pr_crit("The source page directory is null.\n");
        return -1;
    }

    // Initialize iterators for both source and destination page directories.
    page_iterator_t src_iter;
    page_iterator_t dst_iter;

    // Initialize the source iterator to iterate through the source page directory.
    if (__pg_iter_init(&src_iter, src_pgd, src_start, size, flags) < 0) {
        pr_crit("Failed to initialize source page iterator\n");
        return -1;
    }

    // Initialize the destination iterator to iterate through the destination page directory.
    if (__pg_iter_init(&dst_iter, dst_pgd, dst_start, size, flags) < 0) {
        pr_crit("Failed to initialize destination page iterator\n");
        return -1;
    }

    // Iterate over the pages in the source and destination page directories.
    while (__pg_iter_has_next(&src_iter) && __pg_iter_has_next(&dst_iter)) {
        pg_iter_entry_t src_it = __pg_iter_next(&src_iter);
        pg_iter_entry_t dst_it = __pg_iter_next(&dst_iter);

        // Check if the source page is marked as copy-on-write (COW).
        if (src_it.entry->kernel_cow) {
            // Clone the page by assigning the address of the source entry to the destination.
            *(uint32_t *)dst_it.entry = (uint32_t)src_it.entry;
            // Mark the destination page as not present.
            dst_it.entry->present     = 0;
        } else {
            // Copy the frame information from the source entry to the destination entry.
            dst_it.entry->frame = src_it.entry->frame;
            // Set the page table flags for the destination entry.
            __set_pg_table_flags(dst_it.entry, flags);
        }

        // Flush the TLB entry for the destination page to ensure the address is
        // updated. It's essential to verify whether this is required in every
        // case.
        paging_flush_tlb_single(dst_it.pfn * PAGE_SIZE);
    }

    return 0;
}

void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    uintptr_t vm_start;

    // Get the current task.
    task_struct *task = scheduler_get_current_process();

    // Get the file descriptor.
    vfs_file_descriptor_t *file_descriptor = fget(fd);
    if (!file_descriptor) {
        pr_err("Invalid file descriptor.\n");
        return NULL;
    }

    // Get the actual file.
    vfs_file_t *file = file_descriptor->file_struct;
    if (!file) {
        pr_err("Invalid file.\n");
        return NULL;
    }

    stat_t file_stat;
    if (vfs_fstat(file, &file_stat) < 0) {
        pr_err("Failed to get file stat.\n");
        return NULL;
    }

    // Ensure the file size is large enough to map
    if ((offset + length) > file_stat.st_size) {
        pr_err("File is too small for the requested mapping.\n");
        return NULL;
    }

    // Check if a specific address was requested for the memory mapping.
    if (addr && vm_area_is_valid(task->mm, (uintptr_t)addr, (uintptr_t)addr + length)) {
        // If the requested address is valid, use it as the starting address.
        vm_start = (uintptr_t)addr;
    } else {
        // Find an empty spot if no specific address was provided or the provided one is invalid.
        if (vm_area_search_free_area(task->mm, length, &vm_start)) {
            pr_err("Failed to find a suitable spot for a new virtual memory "
                   "area.\n");
            return NULL;
        }
    }

    // Allocate the virtual memory area segment.
    vm_area_struct_t *segment =
        vm_area_create(task->mm, vm_start, length, MM_PRESENT | MM_RW | MM_COW | MM_USER, GFP_HIGHUSER);
    if (!segment) {
        pr_err("Failed to allocate virtual memory area segment.\n");
        return NULL;
    }

    // Set the memory flags for the mapping.
    task->mm->mmap_cache->vm_flags = flags;

    // Return the starting address of the newly created memory segment.
    return (void *)segment->vm_start;
}

int sys_munmap(void *addr, size_t length)
{
    // Get the current task.
    task_struct *task = scheduler_get_current_process();

    // Initialize variables.
    vm_area_struct_t *segment;           // The virtual memory area segment.
    unsigned vm_start = (uintptr_t)addr; // Starting address of the memory area to unmap.
    unsigned size;                       // Size of the segment.

    // Iterate through the list of memory mapped areas in reverse order.
    list_for_each_prev_decl(it, &task->mm->mmap_list)
    {
        segment = list_entry(it, vm_area_struct_t, vm_list);

        // Check if the segment is valid.
        if (!segment) {
            pr_crit("Found a NULL area in the mmap list.\n");
            return -1;
        }

        // Compute the size of the current segment.
        size = segment->vm_end - segment->vm_start;

        // Check if the requested address and length match the current segment.
        if ((vm_start == segment->vm_start) && (length == size)) {
            pr_debug("[0x%p:0x%p] Found it, destroying it.\n", (void *)segment->vm_start, (void *)segment->vm_end);

            // Step 6: Destroy the found virtual memory area.
            if (vm_area_destroy(task->mm, segment) < 0) {
                pr_err(
                    "Failed to destroy the virtual memory area at "
                    "[0x%p:0x%p].\n",
                    (void *)segment->vm_start, (void *)segment->vm_end);
                return -1;
            }

            return 0;
        }
    }

    pr_err(
        "No matching memory area found for unmapping at address 0x%p with "
        "length %zu.\n",
        addr, length);
    return 1;
}
