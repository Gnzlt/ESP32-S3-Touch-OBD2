/*
 * Internal RAM reports on the serial log. Internal RAM is the scarce kind on
 * the S3 (DMA, task stacks, the BLE controller); PSRAM has megabytes to spare.
 */
#pragma once

// "ram <where>: N KB free, N KB lowest, N KB largest block"
void mem_checkpoint(const char *where);

// The checkpoint plus every task's unused stack.
void mem_report_tasks(void);
