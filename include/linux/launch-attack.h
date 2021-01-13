#ifndef LAUNCH_ATTACK_H
#define LAUNCH_ATTACK_H

#include <linux/types.h>

typedef struct {
    //move all blocks here continuously; offset from ovmf start
    uint64_t target_block; 
    //blocks to move continously to target_block from left to right; offset from ovmf start
    uint64_t * source_blocks;
    uint64_t source_blocks_len;
    int target_cpuid_call_count;
    int current_cpuid_call_count;
    uint8_t *malicious_stack_content;
    int malicious_stack_content_len;
    bool active;
} launch_attack_config_t;

extern launch_attack_config_t launch_attack_config;
#endif