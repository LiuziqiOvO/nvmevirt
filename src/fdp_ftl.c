// SPDX-License-Identifier: GPL-2.0-only
/*
 * @Author: ziqi Liu 1021578619@qq.com
 * @Date: 2025-07-24 14:42:04
 * @LastEditors: ziqi Liu 1021578619@qq.com
 * @LastEditTime: 2025-07-24 14:42:04
 * @FilePath: /nvmevirt/src/fdp_ftl.c 
 * @Description: base on fdp_ftl.c, add fdp support
 * Copyright (c) 2025 by ziqi Liu, All Rights Reserved. 
 */

#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "../include/nvmev.h"
#include "../include/fdp_ftl.h"

static inline bool last_pg_in_wordline(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static bool should_gc(struct fdp_ftl *fdp_ftl)
{
	return (fdp_ftl->lm.free_line_cnt <= fdp_ftl->cp.gc_thres_lines);
}

static inline bool should_gc_high(struct fdp_ftl *fdp_ftl)
{
	return fdp_ftl->lm.free_line_cnt <= fdp_ftl->cp.gc_thres_lines_high;
}

static inline struct ppa get_maptbl_ent(struct fdp_ftl *fdp_ftl, uint64_t lpn)
{
	return fdp_ftl->maptbl[lpn];
}

static inline void set_maptbl_ent(struct fdp_ftl *fdp_ftl, uint64_t lpn, struct ppa *ppa)
{
	NVMEV_ASSERT(lpn < fdp_ftl->ssd->sp.tt_pgs);
	fdp_ftl->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	uint64_t pgidx;

	NVMEV_DEBUG_VERBOSE("%s: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", __func__, ppa->g.ch,
			    ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

	pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
		ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

	NVMEV_ASSERT(pgidx < spp->tt_pgs);

	return pgidx;
}

static inline uint64_t get_rmap_ent(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(fdp_ftl, ppa);

	return fdp_ftl->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct fdp_ftl *fdp_ftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(fdp_ftl, ppa);

	fdp_ftl->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
	return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct line *)a)->pos = pos;
}

static inline void consume_write_credit(struct fdp_ftl *fdp_ftl)
{
	fdp_ftl->wfc.write_credits--;
}

static void foreground_gc(struct fdp_ftl *fdp_ftl);

static inline void check_and_refill_write_credit(struct fdp_ftl *fdp_ftl)
{
	struct write_flow_control *wfc = &(fdp_ftl->wfc);
	if (wfc->write_credits <= 0) {
		foreground_gc(fdp_ftl);

		wfc->write_credits += wfc->credits_to_refill;
	}
}

static void init_lines(struct fdp_ftl *fdp_ftl)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct line_mgmt *lm = &fdp_ftl->lm;
	struct line *line;
	int i;

	lm->tt_lines = spp->blks_per_pl;
	NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
					 victim_line_set_pri, victim_line_get_pos,
					 victim_line_set_pos);

	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct line){
			.id = i,
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};

		/* initialize all the lines as free lines */
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static void remove_lines(struct fdp_ftl *fdp_ftl)
{
	pqueue_free(fdp_ftl->lm.victim_line_pq);
	vfree(fdp_ftl->lm.lines);
}

static void init_write_flow_control(struct fdp_ftl *fdp_ftl)
{
	struct write_flow_control *wfc = &(fdp_ftl->wfc);
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
}

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct fdp_ftl *fdp_ftl)
{
	struct line_mgmt *lm = &fdp_ftl->lm;
	struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

	if (!curline) {
		NVMEV_ERROR("No free line left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	NVMEV_DEBUG("%s: free_line_cnt %d\n", __func__, lm->free_line_cnt);
	return curline;
}

// FDP: 从 dsmgmt 字段提取 Placement ID
static inline uint32_t __extract_placement_id(uint32_t dsmgmt)
{
	// Placement ID 通常在 dsmgmt 的低位
	// 这里采用简单的映射：使用低4位作为 RU ID
	return dsmgmt & 0xF;
}

// FDP: 获取指定 RU 的 write pointer
static struct write_pointer *__get_wp_for_ru(struct fdp_ftl *ftl, uint32_t ru_id)
{
	if (!ftl->cp.fdp_enabled) {
		// 非 FDP 模式，使用第一个 WP
		return &ftl->wp_array[0];
	}

	if (ru_id >= ftl->cp.ru_count) {
		// 无效的 RU ID，使用默认的
		ru_id = 0;
	}

	return &ftl->wp_array[ru_id];
}

static struct write_pointer *__get_wp(struct fdp_ftl *ftl, uint32_t io_type)
{
	if (io_type == GC_IO) {
		return &ftl->gc_wp;
	}

	// 对于 USER_IO，返回默认的第一个 RU 的 WP（兼容模式）
	return &ftl->wp_array[0];
}

static void prepare_write_pointer(struct fdp_ftl *fdp_ftl, uint32_t io_type)
{
	struct write_pointer *wp;
	struct line *curline;

	if (io_type == GC_IO) {
		wp = &fdp_ftl->gc_wp;
		curline = get_next_free_line(fdp_ftl);
		if (curline) {
			curline->ru_id = 0; // GC 使用 RU 0
		}
	} else {
		// 为每个 RU 初始化 WP
		uint32_t ru_count = fdp_ftl->cp.fdp_enabled ? fdp_ftl->cp.ru_count : 1;
		uint32_t ru_id;
		for (ru_id = 0; ru_id < ru_count; ru_id++) {
			wp = &fdp_ftl->wp_array[ru_id];
			curline = get_next_free_line(fdp_ftl);

			if (!curline) {
				NVMEV_ERROR("Failed to get free line for RU %d\n", ru_id);
				break;
			}

			// 标记这个 line 属于哪个 RU
			curline->ru_id = ru_id;

			/* wp->curline is always our next-to-write super-block */
			*wp = (struct write_pointer){
				.curline = curline,
				.ch = 0,
				.lun = 0,
				.pg = 0,
				.blk = curline->id,
				.pl = 0,
				.ru_id = ru_id,
			};
		}
		return;
	}

	if (!wp || !curline) {
		NVMEV_ERROR("Failed to prepare write pointer\n");
		return;
	}

	/* wp->curline is always our next-to-write super-block */
	*wp = (struct write_pointer){
		.curline = curline,
		.ch = 0,
		.lun = 0,
		.pg = 0,
		.blk = curline->id,
		.pl = 0,
		.ru_id = 0,
	};
}

static struct ppa get_new_page(struct fdp_ftl *fdp_ftl, uint32_t io_type)
{
	struct ppa ppa;
	struct write_pointer *wp = __get_wp(fdp_ftl, io_type);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

// FDP: 为指定 RU 获取新页面
static struct ppa get_new_page_for_ru(struct fdp_ftl *fdp_ftl, uint32_t ru_id)
{
	struct ppa ppa;
	struct write_pointer *wp = __get_wp_for_ru(fdp_ftl, ru_id);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

static void advance_write_pointer(struct fdp_ftl *fdp_ftl, uint32_t io_type)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct line_mgmt *lm = &fdp_ftl->lm;
	struct write_pointer *wpp = __get_wp(fdp_ftl, io_type);

	NVMEV_DEBUG_VERBOSE("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", wpp->ch, wpp->lun,
			    wpp->pl, wpp->blk, wpp->pg);

	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0)
		goto out;

	wpp->pg -= spp->pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->nchs);
	wpp->ch++;
	if (wpp->ch != spp->nchs)
		goto out;

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch)
		goto out;

	wpp->lun = 0;
	/* go to next wordline in the block */
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk)
		goto out;

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == spp->pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG_VERBOSE("wpp: move line to full_line_list\n");
	} else {
		NVMEV_DEBUG_VERBOSE("wpp: line is moved to victim list\n");
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
	wpp->curline = get_next_free_line(fdp_ftl);
	NVMEV_DEBUG_VERBOSE("wpp: got new clean line %d\n", wpp->curline->id);

	// FDP: 新line继承当前WP的RU ID
	wpp->curline->ru_id = wpp->ru_id;
	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG_VERBOSE("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			    wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

// FDP: 推进指定 RU 的写指针
static void advance_write_pointer_for_ru(struct fdp_ftl *fdp_ftl, uint32_t ru_id)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct line_mgmt *lm = &fdp_ftl->lm;
	struct write_pointer *wpp = __get_wp_for_ru(fdp_ftl, ru_id);

	NVMEV_DEBUG_VERBOSE("RU%d current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", ru_id,
			    wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0)
		goto out;

	wpp->pg -= spp->pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->nchs);
	wpp->ch++;
	if (wpp->ch != spp->nchs)
		goto out;

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch)
		goto out;

	wpp->lun = 0;
	/* go to next wordline in the block */
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk)
		goto out;

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == spp->pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG_VERBOSE("RU%d wpp: move line to full_line_list\n", ru_id);
	} else {
		NVMEV_DEBUG_VERBOSE("RU%d wpp: line is moved to victim list\n", ru_id);
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
	wpp->curline = get_next_free_line(fdp_ftl);
	NVMEV_DEBUG_VERBOSE("RU%d wpp: got new clean line %d\n", ru_id, wpp->curline->id);

	// 标记新line属于当前RU
	wpp->curline->ru_id = ru_id;
	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG_VERBOSE("RU%d advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			    ru_id, wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

static void init_maptbl(struct fdp_ftl *fdp_ftl)
{
	int i;
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	fdp_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		fdp_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
}

static void remove_maptbl(struct fdp_ftl *fdp_ftl)
{
	vfree(fdp_ftl->maptbl);
}

static void init_rmap(struct fdp_ftl *fdp_ftl)
{
	int i;
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	fdp_ftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		fdp_ftl->rmap[i] = INVALID_LPN;
	}
}

static void remove_rmap(struct fdp_ftl *fdp_ftl)
{
	vfree(fdp_ftl->rmap);
}

static void conv_init_ftl(struct fdp_ftl *fdp_ftl, struct convparams *cpp, struct ssd *ssd)
{
	/*copy convparams*/
	fdp_ftl->cp = *cpp;

	fdp_ftl->ssd = ssd;

	/* initialize maptbl */
	init_maptbl(fdp_ftl); // mapping table

	/* initialize rmap */
	init_rmap(fdp_ftl); // reverse mapping table (?)

	/* initialize all the lines */
	init_lines(fdp_ftl);

	/* initialize write pointer, this is how we allocate new pages for writes */
	prepare_write_pointer(fdp_ftl, USER_IO);
	prepare_write_pointer(fdp_ftl, GC_IO);

	init_write_flow_control(fdp_ftl);

	/* FDP: 初始化 WAF 统计 */
	fdp_ftl->waf.external_writes = 0;
	fdp_ftl->waf.internal_writes = 0;

	NVMEV_INFO("Init FTL instance with %d channels (%ld pages)\n", fdp_ftl->ssd->sp.nchs,
		   fdp_ftl->ssd->sp.tt_pgs);

	return;
}

static void conv_remove_ftl(struct fdp_ftl *fdp_ftl)
{
	remove_lines(fdp_ftl);
	remove_rmap(fdp_ftl);
	remove_maptbl(fdp_ftl);
}

static void conv_init_params(struct convparams *cpp)
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
	cpp->enable_gc_delay = 1;
	cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100);

	/* FDP: 默认配置 */
	cpp->fdp_enabled = false; /* 默认禁用，可通过参数启用 */
#ifdef CONFIG_NVMEVIRT_FDP_ENABLED
	cpp->fdp_enabled = true; /* 通过 Kbuild 配置启用 FDP */
#endif
	cpp->ru_count = DEFAULT_RU_COUNT;
	cpp->ru_size_mb = 512; /* 默认512MB per RU */
}

void fdp_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct convparams cpp;
	struct fdp_ftl *conv_ftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	ssd_init_params(&spp, size, nr_parts);
	conv_init_params(&cpp);

	conv_ftls = kmalloc(sizeof(struct fdp_ftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
		ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
		ssd_init(ssd, &spp, cpu_nr_dispatcher);
		conv_init_ftl(&conv_ftls[i], &cpp, ssd);
	}

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		kfree(conv_ftls[i].ssd->pcie->perf_model);
		kfree(conv_ftls[i].ssd->pcie);
		kfree(conv_ftls[i].ssd->write_buffer);

		conv_ftls[i].ssd->pcie = conv_ftls[0].ssd->pcie;
		conv_ftls[i].ssd->write_buffer = conv_ftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)conv_ftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = fdp_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
		   size, ns->size, cpp.pba_pcent);

	return;
}

void fdp_remove_namespace(struct nvmev_ns *ns)
{
	struct fdp_ftl *conv_ftls = (struct fdp_ftl *)ns->ftls;
	const uint32_t nr_parts = SSD_PARTITIONS;
	uint32_t i;

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		/*
		 * These were freed from conv_init_namespace() already.
		 * Mark these NULL so that ssd_remove() skips it.
		 */
		conv_ftls[i].ssd->pcie = NULL;
		conv_ftls[i].ssd->write_buffer = NULL;
	}

	for (i = 0; i < nr_parts; i++) {
		conv_remove_ftl(&conv_ftls[i]);
		ssd_remove(conv_ftls[i].ssd);
		kfree(conv_ftls[i].ssd);
	}

	kfree(conv_ftls);
	ns->ftls = NULL;
}

static inline bool valid_ppa(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	//int sec = ppa->g.sec;

	if (ch < 0 || ch >= spp->nchs)
		return false;
	if (lun < 0 || lun >= spp->luns_per_ch)
		return false;
	if (pl < 0 || pl >= spp->pls_per_lun)
		return false;
	if (blk < 0 || blk >= spp->blks_per_pl)
		return false;
	if (pg < 0 || pg >= spp->pgs_per_blk)
		return false;

	return true;
}

static inline bool valid_lpn(struct fdp_ftl *fdp_ftl, uint64_t lpn)
{
	return (lpn < fdp_ftl->ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct line *get_line(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	return &(fdp_ftl->lm.lines[ppa->g.blk]);
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct line_mgmt *lm = &fdp_ftl->lm;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct line *line;

	/* update corresponding page status */
	pg = get_pg(fdp_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(fdp_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	line = get_line(fdp_ftl, ppa);
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
	if (line->vpc == spp->pgs_per_line) {
		NVMEV_ASSERT(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
		/* Note that line->vpc will be updated by this call */
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		line->vpc--;
	}

	if (was_full_line) {
		/* move line: "full" -> "victim" */
		list_del_init(&line->entry);
		lm->full_line_cnt--;
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}
}

static void mark_page_valid(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	/* update page status */
	pg = get_pg(fdp_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_FREE);
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(fdp_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
	blk->vpc++;

	/* update corresponding line status */
	line = get_line(fdp_ftl, ppa);
	NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
	line->vpc++;
}

static void mark_block_free(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct nand_block *blk = get_blk(fdp_ftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	for (i = 0; i < spp->pgs_per_blk; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	/* reset block status */
	NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

static void gc_read_page(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct convparams *cpp = &fdp_ftl->cp;
	/* advance fdp_ftl status, we don't care about how long it takes */
	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz,
			.interleave_pci_dma = false,
			.ppa = ppa,
		};
		ssd_advance_nand(fdp_ftl->ssd, &gcr);
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct fdp_ftl *fdp_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct convparams *cpp = &fdp_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(fdp_ftl, old_ppa);

	NVMEV_ASSERT(valid_lpn(fdp_ftl, lpn));
	new_ppa = get_new_page(fdp_ftl, GC_IO);
	/* update maptbl */
	set_maptbl_ent(fdp_ftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(fdp_ftl, lpn, &new_ppa);

	mark_page_valid(fdp_ftl, &new_ppa);

	/* need to advance the write pointer here */
	advance_write_pointer(fdp_ftl, GC_IO);

	/* FDP: 统计内部写入（GC） */
	if (fdp_ftl->cp.fdp_enabled) {
		fdp_ftl->waf.internal_writes++;
	}

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &new_ppa,
		};
		if (last_pg_in_wordline(fdp_ftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(fdp_ftl->ssd, &gcw);
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(fdp_ftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(fdp_ftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

	return 0;
}

static struct line *select_victim_line(struct fdp_ftl *fdp_ftl, bool force)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct line_mgmt *lm = &fdp_ftl->lm;
	struct line *victim_line = NULL;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line) {
		return NULL;
	}

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8))) {
		return NULL;
	}

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;

	/* victim_line is a danggling node now */
	return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;
	int pg;

	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
		ppa->g.pg = pg;
		pg_iter = get_pg(fdp_ftl->ssd, ppa);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID) {
			gc_read_page(fdp_ftl, ppa);
			/* delay the maptbl update until "write" happens */
			gc_write_page(fdp_ftl, ppa);
			cnt++;
		}
	}

	NVMEV_ASSERT(get_blk(fdp_ftl->ssd, ppa)->vpc == cnt);
}

/* here ppa identifies the block we want to clean */
static void clean_one_flashpg(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct convparams *cpp = &fdp_ftl->cp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0, i = 0;
	uint64_t completed_time = 0;
	struct ppa ppa_copy = *ppa;

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(fdp_ftl->ssd, &ppa_copy);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID)
			cnt++;

		ppa_copy.g.pg++;
	}

	ppa_copy = *ppa;

	if (cnt <= 0)
		return;

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz * cnt,
			.interleave_pci_dma = false,
			.ppa = &ppa_copy,
		};
		completed_time = ssd_advance_nand(fdp_ftl->ssd, &gcr);
	}

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(fdp_ftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
			gc_write_page(fdp_ftl, &ppa_copy);
		}

		ppa_copy.g.pg++;
	}
}

static void mark_line_free(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct line_mgmt *lm = &fdp_ftl->lm;
	struct line *line = get_line(fdp_ftl, ppa);
	line->ipc = 0;
	line->vpc = 0;
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
}

static int do_gc(struct fdp_ftl *fdp_ftl, bool force)
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct ppa ppa;
	int flashpg;

	victim_line = select_victim_line(fdp_ftl, force);
	if (!victim_line) {
		return -1;
	}

	ppa.g.blk = victim_line->id;
	NVMEV_DEBUG_VERBOSE("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
			    victim_line->ipc, victim_line->vpc, fdp_ftl->lm.victim_line_cnt,
			    fdp_ftl->lm.full_line_cnt, fdp_ftl->lm.free_line_cnt);

	fdp_ftl->wfc.credits_to_refill = victim_line->ipc;

	/* copy back valid data */
	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		int ch, lun;

		ppa.g.pg = flashpg * spp->pgs_per_flashpg;
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;

				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;
				lunp = get_lun(fdp_ftl->ssd, &ppa);
				clean_one_flashpg(fdp_ftl, &ppa);

				if (flashpg == (spp->flashpgs_per_blk - 1)) {
					struct convparams *cpp = &fdp_ftl->cp;

					mark_block_free(fdp_ftl, &ppa);

					if (cpp->enable_gc_delay) {
						struct nand_cmd gce = {
							.type = GC_IO,
							.cmd = NAND_ERASE,
							.stime = 0,
							.interleave_pci_dma = false,
							.ppa = &ppa,
						};
						ssd_advance_nand(fdp_ftl->ssd, &gce);
					}

					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}

	/* update line status */
	mark_line_free(fdp_ftl, &ppa);

	return 0;
}

static void foreground_gc(struct fdp_ftl *fdp_ftl)
{
	if (should_gc_high(fdp_ftl)) {
		NVMEV_DEBUG_VERBOSE("should_gc_high passed");
		/* perform GC here until !should_gc(fdp_ftl) */
		do_gc(fdp_ftl, true);
	}
}

static bool is_same_flash_page(struct fdp_ftl *fdp_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

static bool conv_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct fdp_ftl *conv_ftls = (struct fdp_ftl *)ns->ftls;
	struct fdp_ftl *fdp_ftl = &conv_ftls[0];
	/* spp are shared by all instances*/
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint32_t xfer_size, i;
	uint32_t nr_parts = ns->nr_parts;

	struct ppa prev_ppa;
	struct nand_cmd srd = {
		.type = USER_IO,
		.cmd = NAND_READ,
		.stime = nsecs_start,
		.interleave_pci_dma = true,
	};

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn,
			    nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
			    start_lpn, spp->tt_pgs);
		return false;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
		srd.stime += spp->fw_4kb_rd_lat;
	} else {
		srd.stime += spp->fw_rd_lat;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
		fdp_ftl = &conv_ftls[start_lpn % nr_parts];
		xfer_size = 0;
		prev_ppa = get_maptbl_ent(fdp_ftl, start_lpn / nr_parts);

		/* normal IO read path */
		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
			cur_ppa = get_maptbl_ent(fdp_ftl, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(fdp_ftl, &cur_ppa)) {
				NVMEV_DEBUG_VERBOSE("lpn 0x%llx not mapped to valid ppa\n",
						    local_lpn);
				NVMEV_DEBUG_VERBOSE("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
						    cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk,
						    cur_ppa.g.pl, cur_ppa.g.pg);
				continue;
			}

			// aggregate read io in same flash page
			if (mapped_ppa(&prev_ppa) &&
			    is_same_flash_page(fdp_ftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue;
			}

			if (xfer_size > 0) {
				srd.xfer_size = xfer_size;
				srd.ppa = &prev_ppa;
				nsecs_completed = ssd_advance_nand(fdp_ftl->ssd, &srd);
				nsecs_latest = max(nsecs_completed, nsecs_latest);
			}

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
		}

		// issue remaining io
		if (xfer_size > 0) {
			srd.xfer_size = xfer_size;
			srd.ppa = &prev_ppa;
			nsecs_completed = ssd_advance_nand(fdp_ftl->ssd, &srd);
			nsecs_latest = max(nsecs_completed, nsecs_latest);
		}
	}

	ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}

static bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct fdp_ftl *conv_ftls = (struct fdp_ftl *)ns->ftls;
	struct fdp_ftl *fdp_ftl = &conv_ftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct buffer *wbuf = fdp_ftl->ssd->write_buffer;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;

	struct nand_cmd swr = {
		.type = USER_IO,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,
	};

	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn,
			    nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
			    start_lpn, spp->tt_pgs);
		return false;
	}

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
		return false;

	nsecs_latest =
		ssd_advance_write_buffer(fdp_ftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		uint64_t local_lpn;
		uint64_t nsecs_completed = 0;
		struct ppa ppa;

		fdp_ftl = &conv_ftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;
		ppa = get_maptbl_ent(
			fdp_ftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(fdp_ftl, &ppa);
			set_rmap_ent(fdp_ftl, INVALID_LPN, &ppa);
			NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(fdp_ftl, &ppa));
		}

		/* new write */
		// FDP: 检查是否有placement hint并使用相应的RU
		if (fdp_ftl->cp.fdp_enabled && cmd->rw.dsmgmt) {
			uint32_t ru_id = __extract_placement_id(cmd->rw.dsmgmt);
			ppa = get_new_page_for_ru(fdp_ftl, ru_id);
			/* 推进对应RU的写指针 */
			advance_write_pointer_for_ru(fdp_ftl, ru_id);
			/* FDP: 统计外部写入 */
			fdp_ftl->waf.external_writes++;
		} else {
			ppa = get_new_page(fdp_ftl, USER_IO);
			/* need to advance the write pointer here */
			advance_write_pointer(fdp_ftl, USER_IO);
			/* FDP: 统计外部写入（兼容模式） */
			if (fdp_ftl->cp.fdp_enabled) {
				fdp_ftl->waf.external_writes++;
			}
		}

		/* update maptbl */
		set_maptbl_ent(fdp_ftl, local_lpn, &ppa);
		NVMEV_DEBUG("%s: got new ppa %lld, ", __func__, ppa2pgidx(fdp_ftl, &ppa));
		/* update rmap */
		set_rmap_ent(fdp_ftl, local_lpn, &ppa);

		mark_page_valid(fdp_ftl, &ppa);

		/* Aggregate write io in flash page */
		if (last_pg_in_wordline(fdp_ftl, &ppa)) {
			swr.ppa = &ppa;

			nsecs_completed = ssd_advance_nand(fdp_ftl->ssd, &swr);
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
						    spp->pgs_per_oneshotpg * spp->pgsz);
		}

		consume_write_credit(fdp_ftl);
		check_and_refill_write_credit(fdp_ftl);
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		/* Wait all flash operations */
		ret->nsecs_target = nsecs_latest;
	} else {
		/* Early completion */
		ret->nsecs_target = nsecs_xfer_completed;
	}
	ret->status = NVME_SC_SUCCESS;

	return true;
}

static void conv_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	uint64_t start, latest;
	uint32_t i;
	struct fdp_ftl *conv_ftls = (struct fdp_ftl *)ns->ftls;

	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
		latest = max(latest, ssd_next_idle_time(conv_ftls[i].ssd));
	}

	NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

bool fdp_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		if (!conv_write(ns, req, ret))
			return false;
		break;
	case nvme_cmd_read:
		if (!conv_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		conv_flush(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
			    nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}
