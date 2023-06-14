/*
 * QEMU Arm RME support
 *
 * Copyright Linaro 2024
 */

#include "qemu/osdep.h"

#include "hw/boards.h"
#include "hw/core/cpu.h"
#include "hw/loader.h"
#include "kvm_arm.h"
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qom/object_interfaces.h"
#include "system/confidential-guest-support.h"
#include "system/kvm.h"
#include "system/runstate.h"

#define TYPE_RME_GUEST "rme-guest"
OBJECT_DECLARE_SIMPLE_TYPE(RmeGuest, RME_GUEST)

#define RME_PAGE_SIZE qemu_real_host_page_size()

struct RmeGuest {
    ConfidentialGuestSupport parent_obj;
    Notifier rom_load_notifier;
    GSList *ram_regions;

    hwaddr ram_base;
    size_t ram_size;
};

OBJECT_DEFINE_SIMPLE_TYPE_WITH_INTERFACES(RmeGuest, rme_guest, RME_GUEST,
                                          CONFIDENTIAL_GUEST_SUPPORT,
                                          { TYPE_USER_CREATABLE }, { })

typedef struct {
    hwaddr base;
    hwaddr size;
} RmeRamRegion;

static RmeGuest *rme_guest;

static int rme_init_ram(RmeRamRegion *ram, Error **errp)
{
    int ret;
    hwaddr start = QEMU_ALIGN_DOWN(ram->base, RME_PAGE_SIZE);
    hwaddr end = QEMU_ALIGN_UP(ram->base + ram->size, RME_PAGE_SIZE);
    struct kvm_cap_arm_rme_init_ipa_args init_args = {
        .init_ipa_base = start,
        .init_ipa_size = end - start,
    };

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_ARM_RME, 0,
                            KVM_CAP_ARM_RME_INIT_IPA_REALM,
                            (intptr_t)&init_args);
    if (ret) {
        error_setg_errno(errp, -ret,
                         "failed to init RAM [0x%"HWADDR_PRIx", 0x%"HWADDR_PRIx")",
                         start, end);
    }

    return ret;
}

static int rme_populate_range(hwaddr base, size_t size, bool measure,
                              Error **errp)
{
    int ret;
    hwaddr start = QEMU_ALIGN_DOWN(base, RME_PAGE_SIZE);
    hwaddr end = QEMU_ALIGN_UP(base + size, RME_PAGE_SIZE);
    struct kvm_cap_arm_rme_populate_realm_args populate_args = {
        .populate_ipa_base = start,
        .populate_ipa_size = end - start,
        .flags = measure ? KVM_ARM_RME_POPULATE_FLAGS_MEASURE : 0,
    };

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_ARM_RME, 0,
                            KVM_CAP_ARM_RME_POPULATE_REALM,
                            (intptr_t)&populate_args);
    if (ret) {
        error_setg_errno(errp, -ret,
                   "failed to populate realm [0x%"HWADDR_PRIx", 0x%"HWADDR_PRIx")",
                   start, end);
    }
    return ret;
}

static void rme_populate_ram_region(gpointer data, gpointer err)
{
    Error **errp = err;
    const RmeRamRegion *region = data;

    if (*errp) {
        return;
    }

    rme_populate_range(region->base, region->size, /* measure */ true, errp);
}

static int rme_init_cpus(Error **errp)
{
    int ret;
    CPUState *cs;

    /*
     * Now that do_cpu_reset() initialized the boot PC and
     * kvm_cpu_synchronize_post_reset() registered it, we can finalize the REC.
     */
    CPU_FOREACH(cs) {
        ret = kvm_arm_vcpu_finalize(ARM_CPU(cs), KVM_ARM_VCPU_REC);
        if (ret) {
            error_setg_errno(errp, -ret, "failed to finalize vCPU");
            return ret;
        }
    }
    return 0;
}

static int rme_create_realm(Error **errp)
{
    int ret;

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_ARM_RME, 0,
                            KVM_CAP_ARM_RME_CREATE_RD);
    if (ret) {
        error_setg_errno(errp, -ret, "failed to create Realm Descriptor");
        return -1;
    }

    if (rme_init_ram(&rme_guest->init_ram, errp)) {
        return -1;
    }

    g_slist_foreach(rme_guest->ram_regions, rme_populate_ram_region, errp);
    g_slist_free_full(g_steal_pointer(&rme_guest->ram_regions), g_free);
    if (*errp) {
        return -1;
    }

    if (rme_init_cpus(errp)) {
        return -1;
    }

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_ARM_RME, 0,
                            KVM_CAP_ARM_RME_ACTIVATE_REALM);
    if (ret) {
        error_setg_errno(errp, -ret, "failed to activate realm");
        return -1;
    }

    kvm_mark_guest_state_protected();
    return 0;
}

static void rme_vm_state_change(void *opaque, bool running, RunState state)
{
    Error *err = NULL;

    if (!running) {
        return;
    }

    if (rme_create_realm(&err)) {
        error_propagate_prepend(&error_fatal, err, "RME: ");
    }
}

static void rme_guest_class_init(ObjectClass *oc, void *data)
{
}

static void rme_guest_init(Object *obj)
{
    if (rme_guest) {
        error_report("a single instance of RmeGuest is supported");
        exit(1);
    }
    rme_guest = RME_GUEST(obj);
}

static void rme_guest_finalize(Object *obj)
{
}

static gint rme_compare_ram_regions(gconstpointer a, gconstpointer b)
{
        const RmeRamRegion *ra = a;
        const RmeRamRegion *rb = b;

        g_assert(ra->base != rb->base);
        return ra->base < rb->base ? -1 : 1;
}

static void rme_rom_load_notify(Notifier *notifier, void *data)
{
    RmeRamRegion *region;
    RomLoaderNotifyData *rom = data;

    if (rom->addr == -1) {
        /*
         * These blobs (ACPI tables) are not loaded into guest RAM at reset.
         * Instead the firmware will load them via fw_cfg and measure them
         * itself.
         */
        return;
    }

    region = g_new0(RmeRamRegion, 1);
    region->base = rom->addr;
    region->size = rom->len;

    /*
     * The Realm Initial Measurement (RIM) depends on the order in which we
     * initialize and populate the RAM regions. To help a verifier
     * independently calculate the RIM, sort regions by GPA.
     */
    rme_guest->ram_regions = g_slist_insert_sorted(rme_guest->ram_regions,
                                                   region,
                                                   rme_compare_ram_regions);
}

int kvm_arm_rme_init(MachineState *ms)
{
    static Error *rme_mig_blocker;
    ConfidentialGuestSupport *cgs = ms->cgs;

    if (!rme_guest) {
        return 0;
    }

    if (!cgs) {
        error_report("missing -machine confidential-guest-support parameter");
        return -EINVAL;
    }

    if (!kvm_check_extension(kvm_state, KVM_CAP_ARM_RME)) {
        return -ENODEV;
    }

    error_setg(&rme_mig_blocker, "RME: migration is not implemented");
    migrate_add_blocker(&rme_mig_blocker, &error_fatal);

    /*
     * The realm activation is done last, when the VM starts, after all images
     * have been loaded and all vcpus finalized.
     */
    qemu_add_vm_change_state_handler(rme_vm_state_change, NULL);

    rme_guest->rom_load_notifier.notify = rme_rom_load_notify;
    rom_add_load_notifier(&rme_guest->rom_load_notifier);

    cgs->require_guest_memfd = true;
    cgs->ready = true;
    return 0;
}

void kvm_arm_rme_init_guest_ram(hwaddr base, size_t size)
{
    if (rme_guest) {
        rme_guest->init_ram.base = base;
        rme_guest->init_ram.size = size;
    }
}

int kvm_arm_rme_vcpu_init(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);

    if (rme_guest) {
        cpu->kvm_rme = true;
        cpu->kvm_init_features[0] |= (1 << KVM_ARM_VCPU_REC);
    }
    return 0;
}

int kvm_arm_rme_vm_type(MachineState *ms)
{
    if (rme_guest) {
        return KVM_VM_TYPE_ARM_REALM;
    }
    return 0;
}
