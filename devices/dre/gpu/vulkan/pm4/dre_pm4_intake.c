/* SPDX-License-Identifier: MIT
 * UNVALIDATED structural subset only. No test/compile/GPU execution has run.
 * No MMIO, DMA publication, allocation, free, recursion, libc or capability
 * registration. Successful parsing never sets RAW_IB_SUBMIT authorization.
 */
#include "dre_pm4_intake.h"

struct frame { uint32_t snapshot, at; };
struct context {
    const struct dre_pm4_request *request;
    const struct dre_pm4_ops *ops;
    struct dre_pm4_workspace *work;
    struct dre_pm4_result *result;
    uint32_t current_snapshot, current_word, header, operation;
};
static uint32_t le32(const uint32_t *value)
{
    const uint8_t *p = (const uint8_t *)value;
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t address64(const uint32_t *words)
{ return le32(words) | (uint64_t)le32(words + 1) << 32; }
static uint32_t parity(uint32_t value)
{
    value ^= value >> 16; value ^= value >> 8; value ^= value >> 4;
    return (0x9669u >> (value & 15u)) & 1u;
}
static uint32_t type7(uint32_t opcode, uint32_t words)
{ return DRE_PM4_TYPE7 | opcode << 16 | parity(opcode) << 23 | words | parity(words) << 15; }
static int gpu_range(uint64_t start, uint64_t bytes)
{ return start && bytes && start < (1ull << 48) && bytes <= (1ull << 48) - start; }
static int overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return a < b + bn && b < a + an; } /* called only for checked finite ranges */
static int host_range(const void *pointer, size_t bytes)
{ return pointer && bytes && (uintptr_t)pointer <= UINTPTR_MAX - bytes; }
static int host_overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an;
}
static enum dre_pm4_status fail(struct context *c, enum dre_pm4_status status)
{
    c->result->status = status;
    c->result->failure_snapshot = c->current_snapshot;
    c->result->failure_word = c->current_word;
    c->result->failure_header = c->header;
    c->result->failure_opcode_or_register = c->operation;
    return status;
}
static const struct dre_pm4_resource *resource(struct context *c, uint64_t handle)
{
    for (uint32_t i = 0; i < c->request->resource_count; ++i)
        if (c->request->resources[i].handle == handle) return &c->request->resources[i];
    return NULL;
}
static enum dre_pm4_status resolve(struct context *c, uint64_t address, uint64_t bytes,
    uint32_t access, const struct dre_pm4_resource **out)
{
    *out = NULL;
    if ((address & 3) || !gpu_range(address, bytes)) return fail(c, DRE_PM4_BAD_ADDRESS);
    for (uint32_t i = 0; i < c->request->protected_count; ++i) {
        const struct dre_pm4_range *p = &c->request->protected_ranges[i];
        if (overlaps(address, bytes, p->gpu_va, p->bytes)) return fail(c, DRE_PM4_PROTECTED_ADDRESS);
    }
    for (uint32_t i = 0; i < c->request->resource_count; ++i) {
        const struct dre_pm4_resource *b = &c->request->resources[i];
        if (address >= b->gpu_va && address - b->gpu_va <= b->bytes && bytes <= b->bytes - (address - b->gpu_va)) {
            if ((b->granted_access & access) != access ||
                (b->submitted_access & (access & 3u)) != (access & 3u))
                return fail(c, DRE_PM4_RESOURCE_ACCESS);
            /* Never permit a memory writer to modify any submitted command/
             * shader executable BO, even through a different packet form. */
            if ((access & DRE_PM4_WRITE) && (b->granted_access & DRE_PM4_EXECUTE))
                return fail(c, DRE_PM4_RESOURCE_ACCESS);
            *out = b; return DRE_PM4_STRUCTURAL_SUBSET_ONLY;
        }
    }
    return fail(c, DRE_PM4_RESOURCE_NOT_LISTED);
}
static enum dre_pm4_status reference(struct context *c, uint64_t address, uint64_t bytes,
    uint32_t access, uint32_t kind, uint32_t address_word,
    const struct dre_pm4_resource **bo)
{
    enum dre_pm4_status status = resolve(c, address, bytes, access, bo);
    if (status != DRE_PM4_STRUCTURAL_SUBSET_ONLY) return status;
    if (c->result->references >= c->work->reference_capacity || c->result->references == DRE_PM4_MAX_REFERENCES)
        return fail(c, DRE_PM4_LIMIT);
    struct dre_pm4_reference *ref = &c->work->references[c->result->references++];
    ref->handle = (*bo)->handle; ref->offset = address - (*bo)->gpu_va; ref->bytes = bytes;
    ref->snapshot = c->current_snapshot; ref->packet_word = c->current_word;
    ref->address_word = address_word; ref->kind = kind;
    return DRE_PM4_STRUCTURAL_SUBSET_ONLY;
}
static enum dre_pm4_status capture(struct context *c, const struct dre_pm4_resource *bo,
    uint64_t offset, uint32_t words, uint32_t *index)
{
    if (!bo || !words || words > DRE_PM4_MAX_SINGLE_WORDS || (offset & 3) ||
        offset > bo->bytes || (uint64_t)words * 4 > bo->bytes - offset)
        return fail(c, DRE_PM4_BAD_PACKET_SHAPE);
    if ((bo->granted_access & (DRE_PM4_READ | DRE_PM4_EXECUTE)) != (DRE_PM4_READ | DRE_PM4_EXECUTE) ||
        (bo->granted_access & DRE_PM4_WRITE) || !(bo->submitted_access & DRE_PM4_READ))
        return fail(c, DRE_PM4_RESOURCE_ACCESS);
    for (uint32_t i = 0; i < c->result->snapshots; ++i) {
        const struct dre_pm4_snapshot *s = &c->work->snapshots[i];
        if (s->source_handle == bo->handle && overlaps(offset, (uint64_t)words * 4, s->source_offset, (uint64_t)s->words * 4)) {
            if (s->state == 2 && s->source_offset == offset && s->words == words)
                return fail(c, DRE_PM4_IB_CYCLE);
            return fail(c, DRE_PM4_REPEATED_OR_OVERLAPPING_IB);
        }
    }
    uint32_t page_words = DRE_PM4_SNAPSHOT_PAGE_BYTES / 4;
    uint32_t reserved = (words + page_words - 1) & ~(page_words - 1);
    uint32_t pages = reserved / page_words;
    if (c->result->snapshots == c->work->snapshot_capacity || c->result->snapshots == DRE_PM4_MAX_SNAPSHOTS ||
        words > DRE_PM4_MAX_TOTAL_WORDS - c->result->copied_words ||
        reserved > c->work->word_capacity - c->result->arena_words ||
        pages > DRE_PM4_MAX_SNAPSHOT_PAGES - c->result->snapshot_pages)
        return fail(c, DRE_PM4_LIMIT);
    *index = c->result->snapshots++;
    struct dre_pm4_snapshot *s = &c->work->snapshots[*index];
    s->source_handle = bo->handle; s->source_offset = offset; s->snapshot_lease = 0;
    s->words = words; s->arena_word = c->result->arena_words; s->state = 1; s->reserved = 0;
    c->result->arena_words += reserved; c->result->copied_words += words; c->result->snapshot_pages += pages;
    uint32_t *destination = c->work->word_arena + s->arena_word;
    /* Reserve/count BEFORE the callback. Even a partial-copy/seal failure
     * cannot lose the caller's retained snapshot lease or permit arena reuse. */
    int complete = c->ops->snapshot(c->ops->cookie, c->request->task_generation,
        bo, offset, destination, words, &s->snapshot_lease);
    if (complete != 1 || !s->snapshot_lease) return fail(c, DRE_PM4_SNAPSHOT_FAILED);
    for (uint32_t i = 0; i < *index; ++i)
        if (c->work->snapshots[i].snapshot_lease == s->snapshot_lease)
            return fail(c, DRE_PM4_OWNERSHIP_UNPROVEN);
    if (c->ops->owns_immutable_snapshot(c->ops->cookie, s->snapshot_lease, destination, (size_t)words * 4) != 1)
        return fail(c, DRE_PM4_MUTABLE_SNAPSHOT);
    s->state = 2;
    return DRE_PM4_STRUCTURAL_SUBSET_ONLY;
}
static enum dre_pm4_status walk(struct context *c, uint32_t root)
{
    struct frame stack[DRE_PM4_MAX_DEPTH];
    uint32_t depth = 1; stack[0].snapshot = root; stack[0].at = 0;
    while (depth) {
        struct frame *frame = &stack[depth - 1];
        struct dre_pm4_snapshot *s = &c->work->snapshots[frame->snapshot];
        const uint32_t *words = c->work->word_arena + s->arena_word;
        c->current_snapshot = frame->snapshot; c->current_word = frame->at;
        c->header = 0; c->operation = 0;
        if (frame->at == s->words) { s->state = 3; --depth; continue; }
        if (frame->at > s->words) return fail(c, DRE_PM4_TRUNCATED_PACKET);
        if (c->result->packets == DRE_PM4_MAX_PACKETS) return fail(c, DRE_PM4_LIMIT);
        ++c->result->packets;
        uint32_t header = le32(words + frame->at); c->header = header;
        uint32_t packet_type = header & 0xf0000000u;
        if (packet_type == DRE_PM4_TYPE4) {
            uint32_t count = header & 0x7f, reg = (header >> 8) & 0x7ffff;
            c->operation = reg;
            if (!count || count > s->words - frame->at - 1) return fail(c, DRE_PM4_TRUNCATED_PACKET);
            /* All direct register packets fail. Recognizing a protected
             * window is a diagnostic, never an allowlist for remaining regs. */
            return fail(c, dre_pm4_register_is_privileged(reg, count) ?
                DRE_PM4_PRIVILEGED_REGISTER : DRE_PM4_UNSUPPORTED_REGISTER_PACKET);
        }
        if (packet_type != DRE_PM4_TYPE7) return fail(c, DRE_PM4_BAD_HEADER);
        uint32_t opcode = (header >> 16) & 0x7f, count = header & 0x3fff;
        c->operation = opcode;
        /* Canonical TYPE7 construction checks both odd parity bits and every
         * reserved bit, including bit14 and the upper reserved nibble. */
        if (header != type7(opcode, count)) return fail(c, DRE_PM4_BAD_HEADER);
        if (count > s->words - frame->at - 1) return fail(c, DRE_PM4_TRUNCATED_PACKET);
        const uint32_t *payload = words + frame->at + 1;
        uint32_t packet_word = frame->at;
        frame->at += count + 1;
        if (dre_pm4_opcode_is_privileged(opcode)) return fail(c, DRE_PM4_PRIVILEGED_PACKET);
        if (opcode == DRE_PM4_NOP) continue; /* Payload is data, never executable packets. */
        if (opcode == DRE_PM4_WAIT_MEM_WRITES || opcode == DRE_PM4_WAIT_FOR_IDLE) {
            if (count) return fail(c, DRE_PM4_BAD_PACKET_SHAPE);
            continue;
        }
        const struct dre_pm4_resource *bo = NULL;
        enum dre_pm4_status status;
        if (opcode == DRE_PM4_MEM_WRITE) {
            if (count < 3 || count - 2 > DRE_PM4_MAX_INLINE_WRITE_WORDS)
                return fail(c, DRE_PM4_BAD_PACKET_SHAPE);
            status = reference(c, address64(payload), (uint64_t)(count - 2) * 4,
                DRE_PM4_WRITE, DRE_PM4_MEMORY_WRITE, packet_word + 1, &bo);
            if (status != DRE_PM4_STRUCTURAL_SUBSET_ONLY) return status;
            continue;
        }
        if (opcode == DRE_PM4_MEM_TO_MEM) {
            /* Only the source-proven one-source32-bit copy, with all negate,
             * DOUBLE/wait/unknown bits clear. Summing and other widths fail. */
            if (count != 5 || le32(payload)) return fail(c, DRE_PM4_BAD_PACKET_SHAPE);
            status = reference(c, address64(payload + 1), 4, DRE_PM4_WRITE,
                DRE_PM4_MEMORY_WRITE, packet_word + 2, &bo);
            if (status != DRE_PM4_STRUCTURAL_SUBSET_ONLY) return status;
            status = reference(c, address64(payload + 3), 4, DRE_PM4_READ,
                DRE_PM4_MEMORY_READ, packet_word + 4, &bo);
            if (status != DRE_PM4_STRUCTURAL_SUBSET_ONLY) return status;
            continue;
        }
        if (opcode == DRE_PM4_INDIRECT_BUFFER) {
            if (count != 3 || !le32(payload + 2) || (le32(payload + 2) & ~0xfffffu))
                return fail(c, DRE_PM4_BAD_PACKET_SHAPE);
            uint32_t child_words = le32(payload + 2);
            if (child_words > DRE_PM4_MAX_SINGLE_WORDS || depth == DRE_PM4_MAX_DEPTH ||
                c->result->edges == c->work->edge_capacity || c->result->edges == DRE_PM4_MAX_EDGES)
                return fail(c, DRE_PM4_LIMIT);
            uint64_t address = address64(payload);
            status = reference(c, address, (uint64_t)child_words * 4,
                DRE_PM4_READ | DRE_PM4_EXECUTE, DRE_PM4_IB_READ, packet_word + 1, &bo);
            if (status != DRE_PM4_STRUCTURAL_SUBSET_ONLY) return status;
            uint32_t child = 0;
            status = capture(c, bo, address - bo->gpu_va, child_words, &child);
            if (status != DRE_PM4_STRUCTURAL_SUBSET_ONLY) return status;
            struct dre_pm4_edge *edge = &c->work->edges[c->result->edges++];
            edge->parent_snapshot = frame->snapshot; edge->packet_word = packet_word;
            edge->child_snapshot = child; edge->address_word = packet_word + 1;
            stack[depth].snapshot = child; stack[depth].at = 0; ++depth;
            continue;
        }
        /* Includes draws/dispatch, LOAD_STATE6, SET_DRAW_STATE, predication,
         * conditional/chained IBs, event writes and unknown/future packets. */
        return fail(c, DRE_PM4_UNSUPPORTED_PACKET);
    }
    return DRE_PM4_STRUCTURAL_SUBSET_ONLY;
}
static enum dre_pm4_status workspace(struct context *c)
{
    const struct dre_pm4_request *r = c->request;
    struct dre_pm4_workspace *w = c->work;
    if (w->in_use) return fail(c, DRE_PM4_WORKSPACE_BUSY);
    if (!w->word_capacity || w->word_capacity > DRE_PM4_MAX_ARENA_WORDS ||
        !w->snapshot_capacity || w->snapshot_capacity > DRE_PM4_MAX_SNAPSHOTS ||
        !w->reference_capacity || w->reference_capacity > DRE_PM4_MAX_REFERENCES ||
        !w->edge_capacity || w->edge_capacity > DRE_PM4_MAX_EDGES ||
        ((uintptr_t)w->word_arena & (DRE_PM4_SNAPSHOT_PAGE_BYTES - 1)))
        return fail(c, DRE_PM4_BAD_INPUT);
    const void *p[6] = {w, c->result, w->word_arena, w->snapshots, w->references, w->edges};
    const size_t n[6] = {sizeof(*w), sizeof(*c->result), (size_t)w->word_capacity * 4,
        (size_t)w->snapshot_capacity * sizeof(*w->snapshots),
        (size_t)w->reference_capacity * sizeof(*w->references), (size_t)w->edge_capacity * sizeof(*w->edges)};
    const void *input[5] = {r, c->ops, r->resources, r->roots, r->protected_ranges};
    const size_t in[5] = {sizeof(*r), sizeof(*c->ops), (size_t)r->resource_count * sizeof(*r->resources),
        (size_t)r->root_count * sizeof(*r->roots), (size_t)r->protected_count * sizeof(*r->protected_ranges)};
    for (unsigned i = 0; i < 6; ++i) {
        if (!host_range(p[i], n[i]) || c->ops->owns_workspace(c->ops->cookie, p[i], n[i]) != 1)
            return fail(c, DRE_PM4_OWNERSHIP_UNPROVEN);
        for (unsigned j = 0; j < i; ++j)
            if (host_overlap(p[i], n[i], p[j], n[j])) return fail(c, DRE_PM4_BAD_INPUT);
        for (unsigned j = 0; j < 5; ++j)
            if (!host_range(input[j], in[j]) || host_overlap(p[i], n[i], input[j], in[j]))
                return fail(c, DRE_PM4_BAD_INPUT);
    }
    return DRE_PM4_STRUCTURAL_SUBSET_ONLY;
}
/* Reject aliases BEFORE clearing the result: a malformed invocation must not
 * overwrite its input request, callback table, workspace or previous leases. */
static int result_storage_disjoint(const struct dre_pm4_request *r, const struct dre_pm4_ops *ops,
    const struct dre_pm4_workspace *w, const struct dre_pm4_result *out)
{
    if (!r->resource_count || r->resource_count > DRE_PM4_MAX_BOS ||
        !r->root_count || r->root_count > DRE_PM4_MAX_ROOTS ||
        !r->protected_count || r->protected_count > DRE_PM4_MAX_PROTECTED ||
        !w->word_capacity || w->word_capacity > DRE_PM4_MAX_ARENA_WORDS ||
        (w->word_capacity % (DRE_PM4_SNAPSHOT_PAGE_BYTES / 4)) ||
        !w->snapshot_capacity || w->snapshot_capacity > DRE_PM4_MAX_SNAPSHOTS ||
        !w->reference_capacity || w->reference_capacity > DRE_PM4_MAX_REFERENCES ||
        !w->edge_capacity || w->edge_capacity > DRE_PM4_MAX_EDGES)
        return 0;
    const void *p[10] = {r, ops, w, r->resources, r->roots, r->protected_ranges,
        w->word_arena, w->snapshots, w->references, w->edges};
    const size_t n[10] = {sizeof(*r), sizeof(*ops), sizeof(*w),
        (size_t)r->resource_count * sizeof(*r->resources), (size_t)r->root_count * sizeof(*r->roots),
        (size_t)r->protected_count * sizeof(*r->protected_ranges), (size_t)w->word_capacity * 4,
        (size_t)w->snapshot_capacity * sizeof(*w->snapshots), (size_t)w->reference_capacity * sizeof(*w->references),
        (size_t)w->edge_capacity * sizeof(*w->edges)};
    for (unsigned i = 0; i < 10; ++i)
        if (!host_range(p[i], n[i]) || host_overlap(out, sizeof(*out), p[i], n[i])) return 0;
    return 1;
}
enum dre_pm4_status dre_pm4_intake(const struct dre_pm4_request *r, const struct dre_pm4_ops *ops,
    struct dre_pm4_workspace *w, struct dre_pm4_result *out)
{
    /* These are kernel-private arguments, not directly trusted user pointers.
     * The binding must copy and resolve the public request first. */
    if (!r || !ops || !w || !out || !ops->owns_resource || !ops->owns_protected_ranges ||
        !ops->owns_workspace || !ops->snapshot || !ops->owns_immutable_snapshot ||
        !host_range(out, sizeof(*out)) || ops->owns_workspace(ops->cookie, out, sizeof(*out)) != 1)
        return DRE_PM4_BAD_INPUT;
    if (w->in_use) return DRE_PM4_WORKSPACE_BUSY;
    if (!result_storage_disjoint(r, ops, w, out)) return DRE_PM4_BAD_INPUT;
    /* Clear a fresh result without touching any existing snapshot arena. */
    uint8_t *zero = (uint8_t *)out;
    for (size_t i = 0; i < sizeof(*out); ++i) zero[i] = 0;
    struct context c = {r, ops, w, out, UINT32_MAX, 0, 0, 0};
    out->failure_snapshot = UINT32_MAX;
    if (r->chip_id != DRE_PM4_CHIP_ID) return fail(&c, DRE_PM4_WRONG_CHIP);
    if (r->flags || !r->task_generation || !r->resource_count || r->resource_count > DRE_PM4_MAX_BOS ||
        !r->root_count || r->root_count > DRE_PM4_MAX_ROOTS ||
        !r->protected_count || r->protected_count > DRE_PM4_MAX_PROTECTED)
        return fail(&c, DRE_PM4_BAD_INPUT);
    enum dre_pm4_status status = workspace(&c);
    if (status != DRE_PM4_STRUCTURAL_SUBSET_ONLY) return status;
    w->in_use = 1;
    if (ops->owns_protected_ranges(ops->cookie, r->task_generation, r->protected_ranges, r->protected_count) != 1)
        return fail(&c, DRE_PM4_OWNERSHIP_UNPROVEN);
    for (uint32_t i = 0; i < r->protected_count; ++i)
        if (!gpu_range(r->protected_ranges[i].gpu_va, r->protected_ranges[i].bytes)) return fail(&c, DRE_PM4_BAD_INPUT);
    for (uint32_t i = 0; i < r->resource_count; ++i) {
        const struct dre_pm4_resource *b = &r->resources[i];
        if (!b->handle || !gpu_range(b->gpu_va, b->bytes) || (b->gpu_va | b->bytes) % 4096 ||
            !(b->granted_access & DRE_PM4_READ) || (b->granted_access & ~7u) ||
            ((b->granted_access & DRE_PM4_WRITE) && (b->granted_access & DRE_PM4_EXECUTE)) ||
            !b->submitted_access || (b->submitted_access & ~3u) || (b->submitted_access & ~b->granted_access))
            return fail(&c, DRE_PM4_RESOURCE_ACCESS);
        if (ops->owns_resource(ops->cookie, r->task_generation, b) != 1) return fail(&c, DRE_PM4_OWNERSHIP_UNPROVEN);
        for (uint32_t j = 0; j < i; ++j)
            if (b->handle == r->resources[j].handle || overlaps(b->gpu_va, b->bytes, r->resources[j].gpu_va, r->resources[j].bytes))
                return fail(&c, DRE_PM4_BAD_INPUT);
        for (uint32_t j = 0; j < r->protected_count; ++j)
            if (overlaps(b->gpu_va, b->bytes, r->protected_ranges[j].gpu_va, r->protected_ranges[j].bytes))
                return fail(&c, DRE_PM4_PROTECTED_ADDRESS);
    }
    out->task_generation = r->task_generation;
    for (uint32_t i = 0; i < r->root_count; ++i) {
        const struct dre_pm4_root *root = &r->roots[i];
        c.current_snapshot = UINT32_MAX; c.current_word = 0; c.header = 0; c.operation = 0;
        const struct dre_pm4_resource *bo = resource(&c, root->handle);
        if (root->reserved || !bo) return fail(&c, DRE_PM4_RESOURCE_NOT_LISTED);
        uint32_t index = 0;
        status = capture(&c, bo, root->offset, root->words, &index);
        if (status != DRE_PM4_STRUCTURAL_SUBSET_ONLY) return status;
        out->root_snapshot[out->roots++] = index;
        status = walk(&c, index);
        if (status != DRE_PM4_STRUCTURAL_SUBSET_ONLY) return status;
    }
    /* Recheck all pinned ownership/seals before handing back a structural
     * plan. This is not a substitute for lifetime locks through execution. */
    for (uint32_t i = 0; i < r->resource_count; ++i)
        if (ops->owns_resource(ops->cookie, r->task_generation, &r->resources[i]) != 1) return fail(&c, DRE_PM4_OWNERSHIP_UNPROVEN);
    if (ops->owns_protected_ranges(ops->cookie, r->task_generation, r->protected_ranges, r->protected_count) != 1)
        return fail(&c, DRE_PM4_OWNERSHIP_UNPROVEN);
    for (uint32_t i = 0; i < out->snapshots; ++i) {
        struct dre_pm4_snapshot *s = &w->snapshots[i];
        if (s->state != 3 || ops->owns_immutable_snapshot(ops->cookie, s->snapshot_lease,
            w->word_arena + s->arena_word, (size_t)s->words * 4) != 1) return fail(&c, DRE_PM4_MUTABLE_SNAPSHOT);
    }
    out->status = DRE_PM4_STRUCTURAL_SUBSET_ONLY;
    out->raw_submission_authorized = 0;
    return out->status;
}
const char *dre_pm4_status_name(enum dre_pm4_status status)
{
    switch (status) {
    case DRE_PM4_STRUCTURAL_SUBSET_ONLY: return "STRUCTURAL_SUBSET_ONLY_NOT_RAW_AUTHORIZATION";
    case DRE_PM4_BAD_INPUT: return "BAD_INPUT";
    case DRE_PM4_WRONG_CHIP: return "WRONG_A619_CHIP";
    case DRE_PM4_OWNERSHIP_UNPROVEN: return "ACTUAL_OWNERSHIP_UNPROVEN";
    case DRE_PM4_WORKSPACE_BUSY: return "SNAPSHOT_WORKSPACE_STILL_OWNED";
    case DRE_PM4_LIMIT: return "BOUNDED_INTAKE_LIMIT";
    case DRE_PM4_SNAPSHOT_FAILED: return "SNAPSHOT_COPY_OR_SEAL_FAILED";
    case DRE_PM4_MUTABLE_SNAPSHOT: return "IMMUTABLE_SNAPSHOT_NOT_PROVEN";
    case DRE_PM4_BAD_HEADER: return "INVALID_OR_NONCANONICAL_PM4_HEADER";
    case DRE_PM4_TRUNCATED_PACKET: return "TRUNCATED_PACKET";
    case DRE_PM4_BAD_PACKET_SHAPE: return "UNSUPPORTED_PACKET_SHAPE";
    case DRE_PM4_PRIVILEGED_PACKET: return "PRIVILEGED_PACKET_DENIED";
    case DRE_PM4_PRIVILEGED_REGISTER: return "PRIVILEGED_REGISTER_DENIED";
    case DRE_PM4_UNSUPPORTED_REGISTER_PACKET: return "ALL_TYPE4_REGISTER_WRITES_DENIED";
    case DRE_PM4_UNSUPPORTED_PACKET: return "PACKET_NOT_IN_STRUCTURAL_SUBSET";
    case DRE_PM4_BAD_ADDRESS: return "INVALID_GPU_ADDRESS_RANGE";
    case DRE_PM4_PROTECTED_ADDRESS: return "BOOTSTRAP_OR_PROTECTED_GPU_RANGE";
    case DRE_PM4_RESOURCE_NOT_LISTED: return "ADDRESS_OR_HANDLE_NOT_IN_PINNED_RESOURCE_LIST";
    case DRE_PM4_RESOURCE_ACCESS: return "PINNED_RESOURCE_PERMISSION_REJECTED";
    case DRE_PM4_IB_CYCLE: return "CYCLIC_INDIRECT_BUFFER";
    case DRE_PM4_REPEATED_OR_OVERLAPPING_IB: return "REPEATED_OR_OVERLAPPING_IB_UNSUPPORTED";
    default: return "UNKNOWN_INTAKE_STATUS";
    }
}
