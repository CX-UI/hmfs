#include <asm/tlbflush.h>
#include <asm/pgalloc.h>

#include "hmfs.h"

static pte_t *hmfs_pte_alloc_one_kernel(struct mm_struct *mm, unsigned long addr)
{
	return (pte_t *)__get_free_page((GFP_KERNEL | __GFP_NOTRACK | __GFP_REPEAT | __GFP_ZERO));
}

static int __hmfs_pud_alloc(struct mm_struct *mm, pgd_t *pgd, unsigned long address)
{
	pud_t *new = pud_alloc_one(mm, address);
	if (!new)
		return -ENOMEM;

	smp_wmb(); /* See comment in __pte_alloc */

	spin_lock(&mm->page_table_lock);
	if (pgd_present(*pgd))		/* Another has populated it */
		pud_free(mm, new);
	else
		pgd_populate(mm, pgd, new);
	spin_unlock(&mm->page_table_lock);
	return 0;

}

static pud_t *hmfs_pud_alloc(struct mm_struct *mm, pgd_t *pgd, unsigned long addr)
{
	return (unlikely(pgd_none(*pgd)) && __hmfs_pud_alloc(mm, pgd, addr))?
					NULL: pud_offset(pgd, addr);
}

static int __hmfs_pmd_alloc(struct mm_struct *mm, pud_t *pud, unsigned long address)
{
	pmd_t *new = pmd_alloc_one(mm, address);
	if (!new)
		return -ENOMEM;

	smp_wmb(); /* See comment in __pte_alloc */

	spin_lock(&mm->page_table_lock);
	if (pud_present(*pud))		/* Another has populated it */
		pmd_free(mm, new);
	else
		pud_populate(mm, pud, new);
	spin_unlock(&mm->page_table_lock);
	return 0;
}

static pmd_t *hmfs_pmd_alloc(struct mm_struct *mm, pud_t *pud, unsigned long address)
{
	return (unlikely(pud_none(*pud)) && __hmfs_pmd_alloc(mm, pud, address))?
				NULL: pmd_offset(pud, address);
}

static pte_t *hmfs_pte_alloc(struct mm_struct *mm, pmd_t *pmd, unsigned long address)
{	
	pte_t *new;
	if (likely(!pmd_none(*pmd)))
		return pte_offset_kernel(pmd, address);
	
	new = hmfs_pte_alloc_one_kernel(mm, address);
	if (!new)
		return NULL;

	smp_wmb(); /* See comment in __pte_alloc */

	spin_lock(&mm->page_table_lock);
	if (likely(pmd_none(*pmd))) {	/* Has another populated it ? */
		pmd_populate_kernel(mm, pmd, new);
		new = NULL;
	} else
		VM_BUG_ON(pmd_trans_splitting(*pmd));
	spin_unlock(&mm->page_table_lock);
	if (new)
		pte_free_kernel(mm, new);
	return pte_offset_kernel(pmd, address);
}

static int map_pte_range(struct mm_struct *mm, pmd_t *pmd, unsigned long addr,
				unsigned long end, uint64_t *pfns, const uint8_t seg_type, int *i)
{
	pte_t *pte = hmfs_pte_alloc(mm, pmd, addr);
	uint64_t pfn = pfns[(*i) >> HMFS_BLOCK_SIZE_4K_BITS[seg_type]];
	pfn += (*i) & (HMFS_BLOCK_SIZE_4K[seg_type] - 1);

	if (!pte)
		return -ENOMEM;
	do {
		set_pte_at(mm, addr, pte, pte_mkspecial(pfn_pte(pfn, PAGE_KERNEL)));
		pfn++;
		(*i)++;
		if (!((*i) & (HMFS_BLOCK_SIZE_4K[seg_type] - 1))) {
			pfn = pfns[(*i) >> HMFS_BLOCK_SIZE_4K_BITS[seg_type]];
		}
	} while (pte++, addr += PAGE_SIZE, addr != end);
	return 0;
}

static int map_pmd_range(struct mm_struct *mm, pud_t *pud, unsigned long addr,
				unsigned long end, uint64_t *pfns, const uint8_t seg_type, int *i)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = hmfs_pmd_alloc(mm, pud, addr);
	if (!pmd)
		return -ENOMEM;
	do {
		next = pmd_addr_end(addr, end);
		if (map_pte_range(mm, pmd, addr, next, pfns, seg_type, i))
			return -ENOMEM;
	} while (pmd++, addr = next, addr != end);
	return 0;
}

static int map_pud_range(struct mm_struct *mm, pgd_t *pgd, unsigned long addr,
				unsigned long end, uint64_t *pfns, const uint8_t seg_type, int *i)
{
	pud_t *pud = NULL;
	unsigned long next;

	pud = hmfs_pud_alloc(mm, pgd, addr);
	if (!pud)
		return -ENOMEM;
	do {
		next = pud_addr_end(addr, end);
		if (map_pmd_range(mm, pud, addr, next, pfns, seg_type, i))
			return -ENOMEM;
	} while (pud++, addr = next, addr != end);
	return 0;
}

/* map data blocks with index [start, start + 8) of inode */
int remap_data_blocks_for_write(struct inode *inode, unsigned long st_addr, 
				uint64_t start, uint64_t end)
{
	struct hmfs_sb_info *sbi = HMFS_I_SB(inode);
	struct mm_struct *mm = current->mm;
	const uint8_t seg_type = HMFS_I(inode)->i_blk_type;
	unsigned long ed_addr = st_addr + ((end - start) << HMFS_BLOCK_SIZE_BITS(seg_type));
	void *data_block;
	pgd_t *pgd = pgd_offset(mm, st_addr);
	unsigned long next, addr = st_addr;
	uint64_t buf[8];
	uint64_t *pfns = buf;
	int i = 0;

	while (start < end) {
		data_block = alloc_new_data_block(sbi, inode, start++);
		if (IS_ERR(data_block))
			return PTR_ERR(data_block);
		*pfns++ = pfn_from_vaddr(sbi, data_block);
	}
	pfns = buf;

	flush_cache_vmap(st_addr, ed_addr);
	do {
		next = pgd_addr_end(addr, ed_addr);
		if (map_pud_range(mm, pgd, addr, next, pfns, seg_type, &i))
			return -ENOMEM;
	} while (pgd++, addr = next, addr != end);

	//FIXME: need flush tlb?
	//flush_tlb_kernel_range(st_addr, ed_addr);
	return 0;
}

static inline uint64_t file_block_bitmap_size(uint64_t nr_map_page)
{
	return	nr_map_page >> 3;
}

int vmap_file_range(struct inode *inode)
{
	struct hmfs_inode_info *fi = HMFS_I(inode);
	struct page **pages;
	uint8_t blk_type = fi->i_blk_type;
	size_t size = i_size_read(inode);
	int ret = 0;
	uint64_t old_nr_map_page = 0;
	
	if (fi->rw_addr) {
		unsigned char *bitmap = NULL;
		old_nr_map_page = fi->nr_map_page;
		if (fi->nr_map_page < (ULONG_MAX >> 1))
			fi->nr_map_page <<= 1;
		else
			return 1;

		bitmap = kzalloc(file_block_bitmap_size(fi->nr_map_page), GFP_KERNEL);
		memcpy(bitmap, fi->block_bitmap, fi->bitmap_size);
		fi->bitmap_size = file_block_bitmap_size(fi->nr_map_page);
		kfree(fi->block_bitmap);
		fi->block_bitmap = bitmap;
	} else {
		uint64_t nr_pages;

		if (!size)
			size = (NORMAL_ADDRS_PER_INODE >> 1) << HMFS_BLOCK_SIZE_BITS(blk_type);
		
		nr_pages = (size + HMFS_BLOCK_SIZE[blk_type] - 1) >> HMFS_BLOCK_SIZE_BITS(blk_type);
		if (nr_pages > NORMAL_ADDRS_PER_INODE || (nr_pages << 1) < NORMAL_ADDRS_PER_INODE)
			nr_pages <<= 1;
		else
			nr_pages = NORMAL_ADDRS_PER_INODE;
		fi->nr_map_page = nr_pages << (HMFS_BLOCK_SIZE_BITS(blk_type) - PAGE_SHIFT);
		fi->bitmap_size = file_block_bitmap_size(fi->nr_map_page);
		fi->block_bitmap = kzalloc(fi->bitmap_size, GFP_KERNEL);
	}

	if (!fi->block_bitmap)
		goto out;

	pages = kzalloc(fi->nr_map_page * sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		goto free_bitmap;

	ret = get_file_page_struct(inode, pages, fi->nr_map_page);

	if (ret && ret != -ENODATA)
		goto free_pages;

	if (fi->rw_addr)
		vm_unmap_ram(fi->rw_addr, old_nr_map_page);

	fi->rw_addr = vm_map_ram(pages, fi->nr_map_page, 0, PAGE_KERNEL);
	if (!fi->rw_addr)
		goto free_pages;
	kfree(pages);
	return 0;
free_pages:
	kfree(pages);
free_bitmap:
	kfree(fi->block_bitmap);
out:
	fi->rw_addr = NULL;
	fi->block_bitmap = NULL;
	fi->bitmap_size = 0;
	fi->nr_map_page = 0;
	return 1;
}
