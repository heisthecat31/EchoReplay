#include <string.h>
#include <wchar.h>
// erv_dump <file.echodemo>: what echoreplay_reader.dll makes of a recording (a check, no names hidden).
#include <stdio.h>
#include "erv_api.h"
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    ErvFile* f = erv_open(argv[1]);
    if (!f) { printf("REFUSED: %s\n", erv_last_error()); return 1; }
    const ErvInfo* i = erv_info(f);
    printf("level %016llX gametype %016llX duration %.2f s imported %u recovered %u\n", i->level_id, i->gametype_id,
           i->duration_s, i->imported, i->recovered);
    printf("players %u, player samples %u, body samples %u, states %u, events %u, voice %u\n", i->player_count,
           i->player_sample_count, i->body_sample_count, i->state_count, i->event_count, i->voice_count);
    for (uint32_t k = 0; k < i->player_count; ++k)
        printf("  slot %u team %u name '%s'\n", erv_players(f)[k].slot, erv_players(f)[k].team, erv_players(f)[k].name);
    {
        // distinct body keys and how far each moved
        struct K { uint32_t tag; uint64_t v; unsigned n; float lo[3], hi[3]; };
        K ks[256]; int nk = 0;
        for (uint32_t k = 0; k < i->body_sample_count; ++k) {
            const ErvBodySample& b = erv_body_samples(f)[k];
            int j = 0;
            for (; j < nk; ++j) if (ks[j].tag == b.id_tag && ks[j].v == b.id_value) break;
            if (j == nk) { if (nk == 256) continue; ks[nk] = {b.id_tag, b.id_value, 0, {b.com[0], b.com[1], b.com[2]}, {b.com[0], b.com[1], b.com[2]}}; ++nk; }
            ks[j].n++;
            for (int a = 0; a < 3; ++a) { if (b.com[a] < ks[j].lo[a]) ks[j].lo[a] = b.com[a]; if (b.com[a] > ks[j].hi[a]) ks[j].hi[a] = b.com[a]; }
        }
        for (int j = 0; j < nk && j < 40; ++j)
            printf("  key tag %X id %llX n %u range x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f\n", ks[j].tag, ks[j].v, ks[j].n,
                   ks[j].lo[0], ks[j].hi[0], ks[j].lo[1], ks[j].hi[1], ks[j].lo[2], ks[j].hi[2]);
        unsigned slots[64] = {};
        for (uint32_t k = 0; k < i->player_sample_count; ++k) if (erv_player_samples(f)[k].slot < 64) slots[erv_player_samples(f)[k].slot]++;
        for (int s2 = 0; s2 < 64; ++s2) if (slots[s2]) printf("  player samples slot %d: %u\n", s2, slots[s2]);
    }
    unsigned tags[4] = {};
    for (uint32_t k = 0; k < i->body_sample_count; ++k) tags[erv_body_samples(f)[k].id_tag < 3 ? erv_body_samples(f)[k].id_tag : 3]++;
    printf("body tags: user %u component %u net %u other %u\n", tags[0], tags[1], tags[2], tags[3]);
    for (uint32_t k = 0; k < i->body_sample_count && k < 6; ++k) {
        const ErvBodySample& b = erv_body_samples(f)[k];
        printf("  body t=%.3f tag %u id %llX s%u com %.2f %.2f %.2f\n", b.t, b.id_tag, b.id_value, b.sampled, b.com[0], b.com[1], b.com[2]);
    }
    for (uint32_t k = 0; k < i->player_sample_count && k < 4; ++k) {
        const ErvPlayerSample& p = erv_player_samples(f)[k];
        printf("  player t=%.3f slot %u s%u q %.2f %.2f %.2f %.2f loff %.2f %.2f %.2f\n", p.t, p.slot, p.sampled, p.head_q[0],
               p.head_q[1], p.head_q[2], p.head_q[3], p.l_off[0], p.l_off[1], p.l_off[2]);
    }
    {
        uint32_t rn = 0;
        const ErvRosterRow* rr = erv_roster(f, &rn);
        const bool all = argc > 2 && wcscmp(argv[2], L"--roster") == 0;
        printf("roster rows %u\n", rn);
        for (uint32_t k = 0; k < rn; ++k) {
            const ErvRosterRow& r = rr[k];
            bool changed = all || k == 0;
            for (uint32_t j = k; !changed && j-- > 0;) {
                if (rr[j].entrant == r.entrant && rr[j].platform_id == r.platform_id) {
                    changed = rr[j].team != r.team || rr[j].active != r.active || strcmp(rr[j].name, r.name) != 0;
                    break;
                }
                if (j == 0) changed = true;
            }
            if (changed)
                printf("  roster t=%.1f entrant %u gen %u team %u active %u id %016llx seen %.1f..%.1f '%s'\n", r.t, r.entrant, r.generation, r.team,
                       r.active, (unsigned long long)r.platform_id, r.first_seen, r.last_seen, r.name);
        }
    }
    for (uint32_t k = 0; k < i->voice_count && k < 3; ++k) {
        const ErvVoice& v = erv_voices(f)[k];
        printf("  voice t=%.3f slot %u bytes %u :", v.t, v.slot, v.bytes);
        for (uint32_t b = 0; b < v.bytes && b < 24; ++b) printf(" %02x", v.payload[b]);
        printf("\n");
    }
    for (uint32_t k = 0; k < i->event_count && k < 5; ++k)
        printf("  event t=%.2f kind %u a %X b %X\n", erv_events(f)[k].t, erv_events(f)[k].kind, erv_events(f)[k].a, erv_events(f)[k].b);
    erv_close(f);
    return 0;
}
