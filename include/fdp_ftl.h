/*
 * @Author       : $ {git_name} <${git_email}>
 * @Date         : 2025-06-26 15:08:17
 * @LastEditors  :  ziqi Liu 1021578619@qq.com
 * @LastEditTime : 2025-07-28 09:16:42
 * @FilePath     : /nvmevirt/include/fdp_ftl.h
 * @Description  : 
 */
// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_FDP_FTL_H
#define _NVMEVIRT_FDP_FTL_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"

// FDP 支持的最大 Reclaim Unit 数量
#define MAX_RECLAIM_UNITS 16
#define DEFAULT_RU_COUNT 8

struct convparams {
	uint32_t gc_thres_lines;
	uint32_t gc_thres_lines_high;
	bool enable_gc_delay;

	double op_area_pcent;
	int pba_pcent; /* (physical space / logical space) * 100*/

	// FDP 相关参数
	bool fdp_enabled;
	uint32_t ru_count; /* Number of Reclaim Units */
	uint32_t ru_size_mb; /* RU size in MB */
};

struct line {
	int id; /* line id, the same as corresponding block id */
	int ipc; /* invalid page count in this line */
	int vpc; /* valid page count in this line */
	struct list_head entry;
	/* position in the priority queue for victim lines */
	size_t pos;

	// FDP: which RU this line belongs to
	uint32_t ru_id;
};

/* wp: record next write addr */
struct write_pointer {
	struct line *curline;
	uint32_t ch;
	uint32_t lun;
	uint32_t pg;
	uint32_t blk;
	uint32_t pl;

	// FDP: which RU this WP serves
	uint32_t ru_id;
};

// FDP: WAF 统计结构
struct waf_stats {
	uint64_t external_writes; /* Host writes */
	uint64_t internal_writes; /* GC writes */
};

struct line_mgmt {
	struct line *lines;

	/* free line list, we only need to maintain a list of blk numbers */
	struct list_head free_line_list;
	pqueue_t *victim_line_pq;
	struct list_head full_line_list;

	uint32_t tt_lines;
	uint32_t free_line_cnt;
	uint32_t victim_line_cnt;
	uint32_t full_line_cnt;
};

struct write_flow_control {
	uint32_t write_credits;
	uint32_t credits_to_refill;
};

struct fdp_ftl {
	struct ssd *ssd;

	struct convparams cp;
	struct ppa *maptbl; /* page level mapping table */
	uint64_t *rmap; /* reverse mapptbl, assume it's stored in OOB */
	// struct write_pointer wp;
	// struct write_pointer gc_wp;

	// FDP: 扩展为 WP 数组以支持多个 RU
	struct write_pointer wp_array[MAX_RECLAIM_UNITS]; /* Per-RU write pointers */
	struct write_pointer gc_wp; /* 保持原有的 GC write pointer */

	struct line_mgmt lm;
	struct write_flow_control wfc;

	// FDP: WAF 统计
	struct waf_stats waf;
};

void fdp_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			uint32_t cpu_nr_dispatcher);

void fdp_remove_namespace(struct nvmev_ns *ns);

bool fdp_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);

#endif
