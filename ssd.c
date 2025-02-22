// SPDX-License-Identifier: GPL-2.0-only
#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "ssd.h"

#define DEBUG 1

#define ROUNDDOWN(x, y) ((x) - ((x) % (y)))
#define ROUNDUP(x, y) (((x) + (y) - 1) / (y) * (y))

static inline uint64_t __get_ioclock(struct ssd *ssd)
{
	return cpu_clock(ssd->cpu_nr_dispatcher);
}

void buffer_init(struct buffer *buf, size_t size, struct ssdparams *spp)
{
	spin_lock_init(&buf->lock);
	buf->size = size;
	buf->pg_size = spp->pgsz;
	buf->ppg_size = spp->pgs_per_flashpg * buf->pg_size;
	buf->ppg_per_buf = buf->size / buf->ppg_size;
	buf->pg_per_ppg = spp->pgs_per_flashpg;
	buf->sec_per_pg = spp->secs_per_pg;
	buf->free_pgs_cnt = buf->ppg_per_buf * buf->pg_per_ppg;
	buf->flush_threshold = buf->ppg_per_buf / 2;
	INIT_LIST_HEAD(&buf->free_ppgs);
	INIT_LIST_HEAD(&buf->used_ppgs);
	
	for (int i = 0; i < buf->ppg_per_buf; i++) {
		struct buffer_ppg* block = 
			(struct buffer_ppg*)kmalloc(sizeof(struct buffer_ppg), GFP_KERNEL);
		block->valid = true;
		block->complete_time = 0;
		block->pages = (struct buffer_page*)kmalloc(sizeof(struct buffer_page) * buf->pg_per_ppg, GFP_KERNEL);
		for (int j = 0; j < buf->pg_per_ppg; j++) {
			block->pages[j].lpn = INVALID_LPN;
			block->pages[j].free_secs = buf->sec_per_pg;
			block->pages[j].sectors = kmalloc(sizeof(bool) * buf->sec_per_pg, GFP_KERNEL);
		}
		list_add_tail(&block->list, &buf->free_ppgs);
	}
}

/* get block from buffer that match with lpn return NULL if not found */
static struct buffer_ppg* __buffer_get_ppg(struct buffer *buf, size_t ftl_idx)
{
	struct buffer_ppg *ppg = NULL;
	list_for_each_entry(ppg, &buf->used_ppgs, list) {
		if (ppg->valid && ppg->pg_idx < buf->pg_per_ppg) {
			return ppg;
		}
	}

	return NULL;
}

static struct buffer_page* __buffer_get_page(struct buffer *buf, uint64_t lpn)
{
	struct buffer_ppg *ppg;
	list_for_each_entry(ppg, &buf->used_ppgs, list) {
		if (ppg->valid) {
			for (int i = 0; i < buf->pg_per_ppg; i++)  {
				if (ppg->pages[i].lpn == lpn) {
					return &ppg->pages[i];
				}
			}
		}
	}

	return NULL;
}

/* fill single block of buffer */
static void __buffer_fill_page(struct buffer *buf, uint64_t lpn, uint64_t size, uint64_t offset)
{
	if (size == 0) return;

	while (!spin_trylock(&buf->lock))
		;

	struct buffer_ppg* ppg;
	struct buffer_page *page = __buffer_get_page(buf, lpn);

	if (page == NULL) {
		ppg = __buffer_get_ppg(buf, lpn);
		if (ppg == NULL) { 
			ppg = list_first_entry(&buf->free_ppgs, struct buffer_ppg, list);
			list_move_tail(&ppg->list, &buf->used_ppgs);
		}

		page = &ppg->pages[ppg->pg_idx++];
		buf->free_pgs_cnt--;
	}

	page->lpn = lpn;

	for (size_t i = 0; i < size / LBA_SIZE; i++) {
		size_t idx = i + offset;
		if (!page->sectors[idx]) {
			page->free_secs--;
			page->sectors[idx] = true;
		}
	}

	spin_unlock(&buf->lock);
}

bool buffer_allocate(struct nvmev_ns *ns, uint64_t start_lpn, uint64_t end_lpn, uint64_t start_offset, uint64_t size)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct buffer *buf = NULL;
	struct buffer_page *page = NULL;
	uint32_t nr_parts = SSD_PARTITIONS;
	uint64_t pgsz = spp->pgsz;
	uint64_t s_lpn = start_lpn;
	uint64_t e_lpn = end_lpn;
	uint64_t start_size = min((spp->secs_per_pg - start_offset) * LBA_SIZE, size);
	size_t required_pgs[SSD_PARTITIONS] = {0, };
	size_t ftl_idx;

	for (size_t i = 0; (i < nr_parts) && (s_lpn <= e_lpn); i++, s_lpn += spp->pgs_per_flashpg) {
		ftl_idx = GET_FTL_IDX(s_lpn);
		conv_ftl = &conv_ftls[ftl_idx];
		buf = &conv_ftl->ssd->write_buffer;
		while (!spin_trylock(&buf->lock))
			;
			
		uint64_t local_start_lpn = s_lpn;
		while(local_start_lpn <= e_lpn) {
			uint64_t local_end_lpn = 
				min(end_lpn, ROUNDDOWN(local_start_lpn + spp->pgs_per_flashpg, spp->pgs_per_flashpg) - 1);
			for (uint64_t lpn = local_start_lpn; lpn <= local_end_lpn; lpn++) {
				// NVMEV_INFO("buffer allocate - ftl_idx: %ld, lpn: %lld", ftl_idx, lpn);
				page = __buffer_get_page(buf, lpn);

				if (page == NULL) {
					required_pgs[ftl_idx]++;
				}
			}

			local_start_lpn = ROUNDDOWN(local_start_lpn + spp->pgs_per_flashpg * nr_parts, spp->pgs_per_flashpg);
		}
		
		if (required_pgs[ftl_idx] > buf->free_pgs_cnt) {
			// NVMEV_INFO("buffer allocate failed - ftl_idx: %ld, required_pgs: %ld, free_pgs_cnt: %ld", ftl_idx, required_pgs[ftl_idx], buf->free_pgs_cnt);
			spin_unlock(&buf->lock);
			return false;
		}

		s_lpn = ROUNDDOWN(s_lpn, spp->pgs_per_flashpg);
		spin_unlock(&buf->lock);
	}

	// if (DEBUG == 1) 
	//  	NVMEV_INFO("buffer allocate start_lpn: %llu, end_lpn: %llu, start_offset: %llu, size: %llu", start_lpn, end_lpn, start_offset, size);

	/* handle first page */
	buf = &conv_ftls[GET_FTL_IDX(start_lpn)].ssd->write_buffer;
	__buffer_fill_page(buf, start_lpn++, start_size, start_offset);

	for (size -= start_size; size > pgsz; size -= pgsz) {
		buf = &conv_ftls[GET_FTL_IDX(start_lpn)].ssd->write_buffer;
		__buffer_fill_page(buf, start_lpn++, pgsz, 0);
	}

	/* handle last page */
	buf = &conv_ftls[GET_FTL_IDX(start_lpn)].ssd->write_buffer;
	__buffer_fill_page(buf, start_lpn, size, 0);

	// if (DEBUG == 1) {
	// 	for (size_t i = 0; i < nr_parts; i++) {
	// 		conv_ftl = &conv_ftls[i];
	// 		buf = &conv_ftl->ssd->write_buffer;
	// 		NVMEV_INFO("buffer status - ftl_idx: %d, free_pgs_cnt: %lu, used ppgs: %lu", buf->ftl_idx, buf->free_pgs_cnt, list_count_nodes(&buf->used_ppgs));
	// 	}
	// }

	return true;
}

bool buffer_release(struct buffer *buf, uint64_t complete_time)
{
	while (!spin_trylock(&buf->lock))
		;

	struct buffer_ppg *block, *tmp;
	/* move all marked used blocks to free blocks */
	list_for_each_entry_safe(block, tmp, &buf->used_ppgs, list) {
		if (!block->valid && block->complete_time == complete_time) {
			list_move_tail(&block->list, &buf->free_ppgs);
			block->complete_time = 0;
			block->valid = true;
			for (size_t i = 0; i < block->pg_idx; i++) {
				block->pages[i].lpn = INVALID_LPN;
				block->pages[i].free_secs = buf->sec_per_pg;
				for (size_t j = 0; j < buf->sec_per_pg; j++) {
					block->pages[i].sectors[j] = false;
				}
			}
			buf->free_pgs_cnt += block->pg_idx;
			block->pg_idx = 0;
		}
	}

	// if (DEBUG == 1)
	//  	NVMEV_INFO("buffer released ftl_idx: %d, free buf: %ld", buf->ftl_idx, list_count_nodes(&buf->free_ppgs));

	spin_unlock(&buf->lock);

	return true;
}

void buffer_refill(struct buffer *buf)
{
	while (!spin_trylock(&buf->lock))
		;
	
	struct buffer_ppg *block, *tmp;
	list_for_each_entry_safe(block, tmp, &buf->used_ppgs, list) {
		list_move_tail(&block->list, &buf->free_ppgs);
		block->complete_time = 0;
		block->valid = true;
		for (size_t i = 0; i < block->pg_idx; i++) {
			block->pages[i].lpn = INVALID_LPN;
			block->pages[i].free_secs = buf->sec_per_pg;
			for (size_t j = 0; j < buf->sec_per_pg; j++) {
				block->pages[i].sectors[j] = false;
			}
		}
		buf->free_pgs_cnt += block->pg_idx;
		block->pg_idx = 0;
	}

	spin_unlock(&buf->lock);
}

// TODO: Fix after check conv_ftl
struct buffer_page *buffer_search(struct buffer *buf, uint64_t lpn)
{
	struct buffer_ppg *ppg;
	list_for_each_entry(ppg, &buf->used_ppgs, list) {
		if (ppg->valid) {
			for (int i = 0; i < buf->pg_per_ppg; i++)  {
				if (ppg->pages[i].lpn == lpn) {
					return &ppg->pages[i];
				}
			}
		}
	}

	return NULL;
}

static void check_params(struct ssdparams *spp)
{
	/*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

	//ftl_assert(is_power_of_2(spp->luns_per_ch));
	//ftl_assert(is_power_of_2(spp->nchs));
}

void ssd_init_params(struct ssdparams *spp, uint64_t capacity, uint32_t nparts)
{
	uint64_t blk_size, total_size;

	spp->secsz = LBA_SIZE;
	spp->secs_per_pg = LOGICAL_PAGE_SIZE / LBA_SIZE; // pg == 4KB
	spp->pgsz = LOGICAL_PAGE_SIZE;

	spp->nchs = NAND_CHANNELS;
	spp->pls_per_lun = PLNS_PER_LUN;
	spp->luns_per_ch = LUNS_PER_NAND_CH;
	spp->cell_mode = CELL_MODE;

	/* partitioning SSD by dividing channel*/
	NVMEV_ASSERT((spp->nchs % nparts) == 0);
	spp->nchs /= nparts;
	capacity /= nparts;

	if (BLKS_PER_PLN > 0) {
		/* flashpgs_per_blk depends on capacity */
		spp->blks_per_pl = BLKS_PER_PLN;
		blk_size = DIV_ROUND_UP(capacity, spp->blks_per_pl * spp->pls_per_lun *
							  spp->luns_per_ch * spp->nchs);
	} else {
		NVMEV_ASSERT(BLK_SIZE > 0);
		blk_size = BLK_SIZE;
		spp->blks_per_pl = DIV_ROUND_UP(capacity, blk_size * spp->pls_per_lun *
								  spp->luns_per_ch * spp->nchs);
	}

	NVMEV_ASSERT((ONESHOT_PAGE_SIZE % spp->pgsz) == 0 && (FLASH_PAGE_SIZE % spp->pgsz) == 0);
	NVMEV_ASSERT((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);

	spp->pgs_per_oneshotpg = ONESHOT_PAGE_SIZE / (spp->pgsz);
	spp->oneshotpgs_per_blk = DIV_ROUND_UP(blk_size, ONESHOT_PAGE_SIZE);

	spp->pgs_per_flashpg = FLASH_PAGE_SIZE / (spp->pgsz);
	spp->flashpgs_per_blk = (ONESHOT_PAGE_SIZE / FLASH_PAGE_SIZE) * spp->oneshotpgs_per_blk;

	spp->pgs_per_blk = spp->pgs_per_oneshotpg * spp->oneshotpgs_per_blk;

	spp->write_unit_size = WRITE_UNIT_SIZE;

	spp->pg_4kb_rd_lat[CELL_TYPE_LSB] = NAND_4KB_READ_LATENCY_LSB;
	spp->pg_4kb_rd_lat[CELL_TYPE_MSB] = NAND_4KB_READ_LATENCY_MSB;
	spp->pg_4kb_rd_lat[CELL_TYPE_CSB] = NAND_4KB_READ_LATENCY_CSB;
	spp->pg_rd_lat[CELL_TYPE_LSB] = NAND_READ_LATENCY_LSB;
	spp->pg_rd_lat[CELL_TYPE_MSB] = NAND_READ_LATENCY_MSB;
	spp->pg_rd_lat[CELL_TYPE_CSB] = NAND_READ_LATENCY_CSB;
	spp->pg_wr_lat = NAND_PROG_LATENCY;
	spp->blk_er_lat = NAND_ERASE_LATENCY;
	spp->max_ch_xfer_size = MAX_CH_XFER_SIZE;

	spp->fw_4kb_rd_lat = FW_4KB_READ_LATENCY;
	spp->fw_rd_lat = FW_READ_LATENCY;
	spp->fw_ch_xfer_lat = FW_CH_XFER_LATENCY;
	spp->fw_wbuf_lat0 = FW_WBUF_LATENCY0;
	spp->fw_wbuf_lat1 = FW_WBUF_LATENCY1;

	spp->ch_bandwidth = NAND_CHANNEL_BANDWIDTH;
	spp->pcie_bandwidth = PCIE_BANDWIDTH;

	spp->write_buffer_size = GLOBAL_WB_SIZE / SSD_PARTITIONS;
	spp->write_early_completion = WRITE_EARLY_COMPLETION;

	/* calculated values */
	spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
	spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
	spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
	spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
	spp->tt_secs = spp->secs_per_ch * spp->nchs;

	spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
	spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
	spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
	spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

	spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
	spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
	spp->tt_blks = spp->blks_per_ch * spp->nchs;

	spp->pls_per_ch = spp->pls_per_lun * spp->luns_per_ch;
	spp->tt_pls = spp->pls_per_ch * spp->nchs;

	spp->tt_luns = spp->luns_per_ch * spp->nchs;

	/* line is special, put it at the end */
	spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
	spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
	spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
	spp->tt_lines = spp->blks_per_lun;
	/* TODO: to fix under multiplanes */ // lun size is super-block(line) size

	check_params(spp);

	total_size = (unsigned long)spp->tt_luns * spp->blks_per_lun * spp->pgs_per_blk *
		     spp->secsz * spp->secs_per_pg;
	blk_size = spp->pgs_per_blk * spp->secsz * spp->secs_per_pg;
	NVMEV_INFO(
		"Total Capacity(GiB,MiB)=%llu,%llu chs=%u luns=%lu lines=%lu blk-size(MiB,KiB)=%u,%u line-size(MiB,KiB)=%lu,%lu",
		BYTE_TO_GB(total_size), BYTE_TO_MB(total_size), spp->nchs, spp->tt_luns,
		spp->tt_lines, BYTE_TO_MB(spp->pgs_per_blk * spp->pgsz),
		BYTE_TO_KB(spp->pgs_per_blk * spp->pgsz), BYTE_TO_MB(spp->pgs_per_line * spp->pgsz),
		BYTE_TO_KB(spp->pgs_per_line * spp->pgsz));
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
	int i;
	pg->nsecs = spp->secs_per_pg;
	pg->sec = kmalloc(sizeof(nand_sec_status_t) * pg->nsecs, GFP_KERNEL);
	for (i = 0; i < pg->nsecs; i++) {
		pg->sec[i] = SEC_FREE;
	}
	pg->status = PG_FREE;
}

static void ssd_remove_nand_page(struct nand_page *pg)
{
	kfree(pg->sec);
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
	int i;
	blk->npgs = spp->pgs_per_blk;
	blk->pg = kmalloc(sizeof(struct nand_page) * blk->npgs, GFP_KERNEL);
	for (i = 0; i < blk->npgs; i++) {
		ssd_init_nand_page(&blk->pg[i], spp);
	}
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt = 0;
	blk->wp = 0;
}

static void ssd_remove_nand_blk(struct nand_block *blk)
{
	int i;

	for (i = 0; i < blk->npgs; i++)
		ssd_remove_nand_page(&blk->pg[i]);

	kfree(blk->pg);
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
	int i;
	pl->nblks = spp->blks_per_pl;
	pl->blk = kmalloc(sizeof(struct nand_block) * pl->nblks, GFP_KERNEL);
	for (i = 0; i < pl->nblks; i++) {
		ssd_init_nand_blk(&pl->blk[i], spp);
	}
}

static void ssd_remove_nand_plane(struct nand_plane *pl)
{
	int i;

	for (i = 0; i < pl->nblks; i++)
		ssd_remove_nand_blk(&pl->blk[i]);

	kfree(pl->blk);
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
	int i;
	lun->npls = spp->pls_per_lun;
	lun->pl = kmalloc(sizeof(struct nand_plane) * lun->npls, GFP_KERNEL);
	for (i = 0; i < lun->npls; i++) {
		ssd_init_nand_plane(&lun->pl[i], spp);
	}
	lun->next_lun_avail_time = 0;
	lun->busy = false;
}

static void ssd_remove_nand_lun(struct nand_lun *lun)
{
	int i;

	for (i = 0; i < lun->npls; i++)
		ssd_remove_nand_plane(&lun->pl[i]);

	kfree(lun->pl);
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
	int i;
	ch->nluns = spp->luns_per_ch;
	ch->lun = kmalloc(sizeof(struct nand_lun) * ch->nluns, GFP_KERNEL);
	for (i = 0; i < ch->nluns; i++) {
		ssd_init_nand_lun(&ch->lun[i], spp);
	}

	ch->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
	chmodel_init(ch->perf_model, spp->ch_bandwidth);

	/* Add firmware overhead */
	ch->perf_model->xfer_lat += (spp->fw_ch_xfer_lat * UNIT_XFER_SIZE / KB(4));
}

static void ssd_remove_buffer(struct ssd *ssd)
{
	struct buffer *buf = &ssd->write_buffer;
	struct buffer_ppg *block, *tmp;
	list_for_each_entry_safe(block, tmp, &buf->free_ppgs, list) {
		for (int i = 0; i < buf->pg_per_ppg; i++) {
			kfree(block->pages[i].sectors);
		}
		kfree(block->pages);
		kfree(block);
	}
	list_for_each_entry_safe(block, tmp, &buf->used_ppgs, list) {
		for (int i = 0; i < buf->pg_per_ppg; i++) {
			kfree(block->pages[i].sectors);
		}
		kfree(block->pages);
		kfree(block);
	}
}

static void ssd_remove_ch(struct ssd_channel *ch)
{
	int i;

	kfree(ch->perf_model);

	for (i = 0; i < ch->nluns; i++)
		ssd_remove_nand_lun(&ch->lun[i]);

	kfree(ch->lun);
}

static void ssd_init_pcie(struct ssd_pcie *pcie, struct ssdparams *spp)
{
	pcie->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
	chmodel_init(pcie->perf_model, spp->pcie_bandwidth);
}

static void ssd_remove_pcie(struct ssd_pcie *pcie)
{
	kfree(pcie->perf_model);
}

void ssd_init(struct ssd *ssd, struct ssdparams *spp, uint32_t cpu_nr_dispatcher)
{
	uint32_t i;
	/* copy spp */
	ssd->sp = *spp;

	/* initialize conv_ftl internal layout architecture */
	ssd->ch = kmalloc(sizeof(struct ssd_channel) * spp->nchs, GFP_KERNEL); // 40 * 8 = 320
	for (i = 0; i < spp->nchs; i++) {
		ssd_init_ch(&(ssd->ch[i]), spp);
	}

	/* Set CPU number to use same cpuclock as io.c */
	ssd->cpu_nr_dispatcher = cpu_nr_dispatcher;

	ssd->pcie = kmalloc(sizeof(struct ssd_pcie), GFP_KERNEL);
	ssd_init_pcie(ssd->pcie, spp);

	buffer_init(&ssd->write_buffer, spp->write_buffer_size, spp);

	return;
}

void ssd_remove(struct ssd *ssd)
{
	uint32_t i;

	ssd_remove_buffer(ssd);

	if (ssd->pcie) {
		kfree(ssd->pcie->perf_model);
		kfree(ssd->pcie);
	}

	for (i = 0; i < ssd->sp.nchs; i++) {
		ssd_remove_ch(&(ssd->ch[i]));
	}

	kfree(ssd->ch);
}

uint64_t ssd_advance_pcie(struct ssd *ssd, uint64_t request_time, uint64_t length)
{
	struct channel_model *perf_model = ssd->pcie->perf_model;
	return chmodel_request(perf_model, request_time, length);
}

/* Write buffer Performance Model
  Y = A + (B * X)
  Y : latency (ns)
  X : transfer size (4KB unit)
  A : fw_wbuf_lat0
  B : fw_wbuf_lat1 + pcie dma transfer
*/
uint64_t ssd_advance_write_buffer(struct ssd *ssd, uint64_t request_time, uint64_t length)
{
	uint64_t nsecs_latest = request_time;
	struct ssdparams *spp = &ssd->sp;

	nsecs_latest += spp->fw_wbuf_lat0;
	nsecs_latest += spp->fw_wbuf_lat1 * DIV_ROUND_UP(length, KB(4));

	nsecs_latest = ssd_advance_pcie(ssd, nsecs_latest, length);

	return nsecs_latest;
}

uint64_t ssd_advance_nand(struct ssd *ssd, struct nand_cmd *ncmd)
{
	int c = ncmd->cmd;
	uint64_t cmd_stime = (ncmd->stime == 0) ? __get_ioclock(ssd) : ncmd->stime;
	uint64_t nand_stime, nand_etime;
	uint64_t chnl_stime, chnl_etime;
	uint64_t remaining, xfer_size, completed_time;
	struct ssdparams *spp;
	struct nand_lun *lun;
	struct ssd_channel *ch;
	struct ppa *ppa = ncmd->ppa;
	uint32_t cell;
	NVMEV_DEBUG(
		"SSD: %p, Enter stime: %lld, ch %d lun %d blk %d page %d command %d ppa 0x%llx\n",
		ssd, ncmd->stime, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg, c, ppa->ppa);

	if (ppa->ppa == UNMAPPED_PPA) {
		NVMEV_ERROR("Error ppa 0x%llx - %d\n", ppa->ppa, ncmd->cmd);
		return cmd_stime;
	}

	spp = &ssd->sp;
	lun = get_lun(ssd, ppa);
	ch = get_ch(ssd, ppa);
	cell = get_cell(ssd, ppa);
	remaining = ncmd->xfer_size;

	switch (c) {
	case NAND_READ:
		/* read: perform NAND cmd first */
		nand_stime = max(lun->next_lun_avail_time, cmd_stime);

		if (ncmd->xfer_size == 4096) {
			nand_etime = nand_stime + spp->pg_4kb_rd_lat[cell];
		} else {
			nand_etime = nand_stime + spp->pg_rd_lat[cell];
		}

		/* read: then data transfer through channel */
		chnl_stime = nand_etime;
		// NVMEV_INFO("NAND Latency: %lld\n", nand_etime - nand_stime);

		while (remaining) {
			xfer_size = min(remaining, (uint64_t)spp->max_ch_xfer_size);
			chnl_etime = chmodel_request(ch->perf_model, chnl_stime, xfer_size);

			if (ncmd->interleave_pci_dma) { /* overlap pci transfer with nand ch transfer*/
				completed_time = ssd_advance_pcie(ssd, chnl_etime, xfer_size);
			} else {
				completed_time = chnl_etime;
			}

			remaining -= xfer_size;
			chnl_stime = chnl_etime;
		}

		// NVMEV_INFO("Channel Latency: %lld\n", completed_time - nand_etime);

		lun->next_lun_avail_time = chnl_etime;
		break;

	case NAND_WRITE:
		/* write: transfer data through channel first */
		chnl_stime = max(lun->next_lun_avail_time, cmd_stime);

		chnl_etime = chmodel_request(ch->perf_model, chnl_stime, ncmd->xfer_size);

		/* write: then do NAND program */
		nand_stime = chnl_etime;
		nand_etime = nand_stime + spp->pg_wr_lat;
		lun->next_lun_avail_time = nand_etime;
		completed_time = nand_etime;
		break;

	case NAND_ERASE:
		/* erase: only need to advance NAND status */
		nand_stime = max(lun->next_lun_avail_time, cmd_stime);
		nand_etime = nand_stime + spp->blk_er_lat;
		lun->next_lun_avail_time = nand_etime;
		completed_time = nand_etime;
		break;

	case NAND_NOP:
		/* no operation: just return last completed time of lun */
		nand_stime = max(lun->next_lun_avail_time, cmd_stime);
		lun->next_lun_avail_time = nand_stime;
		completed_time = nand_stime;
		break;

	default:
		NVMEV_ERROR("Unsupported NAND command: 0x%x\n", c);
		return 0;
	}

	return completed_time;
}

uint64_t ssd_next_idle_time(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp;
	uint32_t i, j;
	uint64_t latest = __get_ioclock(ssd);

	for (i = 0; i < spp->nchs; i++) {
		struct ssd_channel *ch = &ssd->ch[i];

		for (j = 0; j < spp->luns_per_ch; j++) {
			struct nand_lun *lun = &ch->lun[j];
			latest = max(latest, lun->next_lun_avail_time);
		}
	}

	return latest;
}

void adjust_ftl_latency(int target, int lat)
{
/* TODO ..*/
#if 0
    struct ssdparams *spp;
    int i;

    for (i = 0; i < SSD_PARTITIONS; i++) {
        spp = &(g_conv_ftls[i].sp);
        NVMEV_INFO("Before latency: %d %d %d, change to %d\n", spp->pg_rd_lat, spp->pg_wr_lat, spp->blk_er_lat, lat);
        switch (target) {
            case NAND_READ:
                spp->pg_rd_lat = lat;
                break;

            case NAND_WRITE:
                spp->pg_wr_lat = lat;
                break;

            case NAND_ERASE:
                spp->blk_er_lat = lat;
                break;

            default:
                NVMEV_ERROR("Unsupported NAND command\n");
        }
        NVMEV_INFO("After latency: %d %d %d\n", spp->pg_rd_lat, spp->pg_wr_lat, spp->blk_er_lat);
    }
#endif
}
