/*
 * rt5650buddy - pilote KMDF minimal pour le codec Realtek RT5650 de l'Acer
 * Chromebase 24 (carte Google BUDDY, Broadwell), sous Windows 10.
 *
 * Le transport audio (DSP Intel SST / SSP0) est assure par le pilote CoolStar
 * SST (csaudiosstcatpt), configure en I2S "provider" 2 canaux, MCLK 24 MHz.
 * Ce pilote-ci se contente de configurer le codec par I2C (ACPI \_SB.PCI0.I2C0,
 * adresse 0x1A) : PLL, format I2S esclave, chemin DAC -> haut-parleurs, ampli.
 *
 * Les valeurs de registres viennent du pilote Linux sound/soc/codecs/rt5645.c
 * (qui gere aussi le RT5650) et de la machine sound/soc/intel/boards/bdw-rt5650.c.
 *
 * Diagnostic : le resultat est ecrit dans la cle "Device Parameters" du
 * peripherique (valeurs DeviceId, InitStatus, InitStep).
 */
#include <ntddk.h>
#include <wdf.h>
#include <spb.h>
#include <reshub.h>

#define RT_TAG 'ltRB'

/* Registres (rt5645.h) */
#define RT5645_RESET          0x00
#define RT5645_SPK_VOL        0x01
#define RT5645_HP_VOL         0x02
#define RT5645_AD_DA_MIXER    0x29
#define RT5645_STO_DAC_MIXER  0x2a
#define RT5650_A_DAC_SOUR     0x2d
#define RT5645_SPK_L_MIXER    0x46
#define RT5645_SPK_R_MIXER    0x47
#define RT5645_SPO_MIXER      0x48
#define RT5645_PWR_DIG1       0x61
#define RT5645_PWR_DIG2       0x62
#define RT5645_PWR_ANLG1      0x63
#define RT5645_PWR_ANLG2      0x64
#define RT5645_PWR_MIXER      0x65
#define RT5645_PWR_VOL        0x66
#define RT5645_PRIV_INDEX     0x6a
#define RT5645_PRIV_DATA      0x6c
#define RT5645_I2S1_SDP       0x70
#define RT5645_ADDA_CLK1      0x73
#define RT5645_GLB_CLK        0x80
#define RT5645_PLL_CTRL1      0x81
#define RT5645_PLL_CTRL2      0x82
#define RT5645_ASRC_4         0x8a
#define RT5645_DEPOP_M2       0x8f
#define RT5645_CLSD_OUT_CTRL  0xa0
#define RT5645_IL_CMD3        0xdd
#define RT5645_GEN_CTRL1      0xfa
#define RT5645_GEN_CTRL3      0xfc
#define RT5645_VENDOR_ID2     0xff

#define RT5650_DEVICE_ID      0x6419

typedef struct _DEVICE_CONTEXT {
    WDFIOTARGET   I2c;
    LARGE_INTEGER ConnectionId;
    BOOLEAN       HaveConnection;
    ULONG         Step;       /* derniere etape atteinte (diagnostic) */
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetCtx)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD RtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE RtEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE RtEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY RtEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT RtEvtD0Exit;

static VOID RtSleepMs(ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * (LONGLONG)ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static NTSTATUS RtWrite(PDEVICE_CONTEXT ctx, UCHAR reg, USHORT val)
{
    UCHAR buf[3];
    WDF_MEMORY_DESCRIPTOR desc;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG_PTR written = 0;

    buf[0] = reg;
    buf[1] = (UCHAR)(val >> 8);
    buf[2] = (UCHAR)(val & 0xff);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, buf, sizeof(buf));
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    opts.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    return WdfIoTargetSendWriteSynchronously(ctx->I2c, NULL, &desc, NULL, &opts, &written);
}

static NTSTATUS RtRead(PDEVICE_CONTEXT ctx, UCHAR reg, USHORT *val)
{
    UCHAR regbuf = reg;
    UCHAR data[2] = { 0, 0 };
    SPB_TRANSFER_LIST_AND_ENTRIES(2) seq;
    WDF_MEMORY_DESCRIPTOR desc;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG_PTR bytes = 0;
    NTSTATUS st;

    SPB_TRANSFER_LIST_INIT(&(seq.List), 2);
    seq.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &regbuf, 1);
    seq.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, data, 2);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, &seq, sizeof(seq));
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    opts.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    st = WdfIoTargetSendIoctlSynchronously(ctx->I2c, NULL, IOCTL_SPB_EXECUTE_SEQUENCE,
                                           &desc, NULL, &opts, &bytes);
    if (NT_SUCCESS(st)) {
        *val = (USHORT)((data[0] << 8) | data[1]);
    }
    return st;
}

static NTSTATUS RtUpdate(PDEVICE_CONTEXT ctx, UCHAR reg, USHORT mask, USHORT val)
{
    USHORT old = 0;
    NTSTATUS st = RtRead(ctx, reg, &old);
    if (!NT_SUCCESS(st)) return st;
    return RtWrite(ctx, reg, (USHORT)((old & ~mask) | (val & mask)));
}

static NTSTATUS RtWritePriv(PDEVICE_CONTEXT ctx, UCHAR idx, USHORT val)
{
    NTSTATUS st = RtWrite(ctx, RT5645_PRIV_INDEX, idx);
    if (!NT_SUCCESS(st)) return st;
    return RtWrite(ctx, RT5645_PRIV_DATA, val);
}

static VOID RtSaveDiag(WDFDEVICE dev, ULONG deviceId, NTSTATUS status, ULONG step)
{
    WDFKEY key;
    DECLARE_CONST_UNICODE_STRING(nId, L"DeviceId");
    DECLARE_CONST_UNICODE_STRING(nSt, L"InitStatus");
    DECLARE_CONST_UNICODE_STRING(nStep, L"InitStep");

    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(dev, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        WdfRegistryAssignULong(key, &nId, deviceId);
        WdfRegistryAssignULong(key, &nSt, (ULONG)status);
        WdfRegistryAssignULong(key, &nStep, step);
        WdfRegistryClose(key);
    }
}

#define CHK(x) do { ctx->Step++; st = (x); if (!NT_SUCCESS(st)) goto out; } while (0)

static NTSTATUS RtCodecInit(WDFDEVICE dev, PDEVICE_CONTEXT ctx)
{
    NTSTATUS st;
    USHORT id = 0;

    ctx->Step = 0;

    /* 1. Identification */
    CHK(RtRead(ctx, RT5645_VENDOR_ID2, &id));
    if (id != RT5650_DEVICE_ID) {
        st = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto out;
    }

    /* 2. Reset + sequences d'init (rt5645_i2c_probe) */
    CHK(RtWrite(ctx, RT5645_RESET, 0));
    RtSleepMs(10);
    CHK(RtWrite(ctx, RT5645_AD_DA_MIXER, 0x8080));
    CHK(RtWritePriv(ctx, 0x3d, 0x3600));
    CHK(RtWritePriv(ctx, 0x1c, 0xfd70));
    CHK(RtWritePriv(ctx, 0x20, 0x611f));
    CHK(RtWritePriv(ctx, 0x21, 0x4040));
    CHK(RtWritePriv(ctx, 0x23, 0x0004));
    CHK(RtWrite(ctx, RT5645_ASRC_4, 0x0120));
    /* rt5650_init_list */
    CHK(RtWrite(ctx, 0xf6, 0x0100));
    CHK(RtWrite(ctx, RT5645_PWR_ANLG1, 0x0002));
    CHK(RtWrite(ctx, RT5645_IL_CMD3, 0x6728));
    CHK(RtUpdate(ctx, RT5645_CLSD_OUT_CTRL, 0x00c0, 0x00c0));

    /* 3. Bias : OFF -> STANDBY -> ON (rt5645_set_bias_level) */
    /* VREF1 | MB | BG | VREF2 */
    CHK(RtUpdate(ctx, RT5645_PWR_ANLG1, 0xa810, 0xa810));
    RtSleepMs(10);
    /* FV1 | FV2 */
    CHK(RtUpdate(ctx, RT5645_PWR_ANLG1, 0x4008, 0x4008));
    CHK(RtWrite(ctx, RT5645_DEPOP_M2, 0x1140));
    RtSleepMs(40);
    /* DIG_GATE_CTRL */
    CHK(RtUpdate(ctx, RT5645_GEN_CTRL1, 0x0001, 0x0001));

    /* 4. Horloges : PLL1 <- MCLK 24 MHz -> 24.576 MHz (approx. 24.571), SYSCLK <- PLL1 */
    CHK(RtUpdate(ctx, RT5645_GLB_CLK, 0x3800, 0x0000));     /* PLL1 src = MCLK */
    CHK(RtWrite(ctx, RT5645_PLL_CTRL1, 0x1481));             /* N=41, K=1 */
    CHK(RtWrite(ctx, RT5645_PLL_CTRL2, 0xc000));             /* M=12, pas de bypass */
    CHK(RtUpdate(ctx, RT5645_PWR_ANLG2, 0x0200, 0x0200));    /* PWR_PLL */
    CHK(RtUpdate(ctx, RT5645_GLB_CLK, 0xc000, 0x4000));     /* SYSCLK = PLL1 */

    /* 5. Interface I2S1 : esclave, I2S, 24 bits (RT5650 : DL en [5:4]),
     *    pre-diviseur 2 (24.576 MHz / (256 * 48 kHz)) */
    CHK(RtUpdate(ctx, RT5645_I2S1_SDP, 0x8000 | 0x0100 | 0x0003 | 0x0030, 0x8000 | 0x0020));
    CHK(RtUpdate(ctx, RT5645_ADDA_CLK1, 0x7000, 0x1000));

    /* 6. Alimentation numerique : I2S1, DAC L1, DAC R1, filtre stereo DAC */
    CHK(RtUpdate(ctx, RT5645_PWR_DIG1, 0x9800, 0x9800));
    CHK(RtUpdate(ctx, RT5645_PWR_DIG2, 0x0800, 0x0800));

    /* 7. Chemin numerique : IF1 DAC -> DAC1 MIX -> (A DAC1 Mux = DAC1) -> DAC L1/R1 */
    CHK(RtWrite(ctx, RT5645_AD_DA_MIXER, 0x8080));           /* ADC mix coupe, DAC1 actif, src IF1 */
    CHK(RtUpdate(ctx, RT5645_STO_DAC_MIXER, 0x4040, 0x0000)); /* DAC L1/R1 vers mixeur stereo */
    CHK(RtUpdate(ctx, RT5650_A_DAC_SOUR, 0x000c, 0x0000));    /* A DAC1 L/R <- DAC1 MIX */

    /* 8. Chemin haut-parleurs : DAC L1/R1 -> SPK MIX -> SPKVOL -> SPOL/SPOR MIX -> ampli classe D */
    CHK(RtWrite(ctx, RT5645_SPK_L_MIXER, 0x003c));           /* DAC L1 seul */
    CHK(RtWrite(ctx, RT5645_SPK_R_MIXER, 0x003c));           /* DAC R1 seul */
    CHK(RtWrite(ctx, RT5645_SPO_MIXER, 0xd806));             /* SPOL<-SPKVOL L, SPOR<-SPKVOL R */
    CHK(RtUpdate(ctx, RT5645_PWR_MIXER, 0x3000, 0x3000));    /* SPK MIX L/R */
    CHK(RtUpdate(ctx, RT5645_PWR_VOL, 0xc000, 0xc000));      /* SPKVOL L/R */
    CHK(RtWrite(ctx, RT5645_SPK_VOL, 0x0808));               /* non mute, ~0 dB */

    /* 9. Ampli (rt5645_spk_event POST_PMU) : CLS_D | CLS_D_R | CLS_D_L, DET_CLK mode 1 */
    CHK(RtUpdate(ctx, RT5645_PWR_DIG1, 0x0301, 0x0301));
    CHK(RtUpdate(ctx, RT5645_GEN_CTRL3, 0x0600, 0x0200));

    st = STATUS_SUCCESS;
out:
    RtSaveDiag(dev, id, st, ctx->Step);
    return st;
}

NTSTATUS RtEvtPrepareHardware(WDFDEVICE dev, WDFCMRESLIST raw, WDFCMRESLIST trans)
{
    PDEVICE_CONTEXT ctx = GetCtx(dev);
    ULONG i, n;
    NTSTATUS st;
    WDF_IO_TARGET_OPEN_PARAMS open;
    WCHAR pathBuf[RESOURCE_HUB_PATH_SIZE];
    UNICODE_STRING path;

    UNREFERENCED_PARAMETER(raw);

    n = WdfCmResourceListGetCount(trans);
    for (i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(trans, i);
        if (d->Type == CmResourceTypeConnection &&
            d->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            d->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C &&
            !ctx->HaveConnection) {
            ctx->ConnectionId.LowPart = d->u.Connection.IdLowPart;
            ctx->ConnectionId.HighPart = d->u.Connection.IdHighPart;
            ctx->HaveConnection = TRUE;
        }
    }
    if (!ctx->HaveConnection) {
        RtSaveDiag(dev, 0, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlInitEmptyUnicodeString(&path, pathBuf, sizeof(pathBuf));
    st = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path, ctx->ConnectionId.LowPart, ctx->ConnectionId.HighPart);
    if (!NT_SUCCESS(st)) return st;

    st = WdfIoTargetCreate(dev, WDF_NO_OBJECT_ATTRIBUTES, &ctx->I2c);
    if (!NT_SUCCESS(st)) return st;

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &path, GENERIC_READ | GENERIC_WRITE);
    st = WdfIoTargetOpen(ctx->I2c, &open);
    if (!NT_SUCCESS(st)) {
        RtSaveDiag(dev, 0, st, 0);
    }
    return st;
}

NTSTATUS RtEvtReleaseHardware(WDFDEVICE dev, WDFCMRESLIST trans)
{
    PDEVICE_CONTEXT ctx = GetCtx(dev);
    UNREFERENCED_PARAMETER(trans);
    if (ctx->I2c) {
        WdfIoTargetClose(ctx->I2c);
        WdfObjectDelete(ctx->I2c);
        ctx->I2c = NULL;
    }
    ctx->HaveConnection = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS RtEvtD0Entry(WDFDEVICE dev, WDF_POWER_DEVICE_STATE prev)
{
    UNREFERENCED_PARAMETER(prev);
    /* Ne jamais faire echouer le demarrage du peripherique : on note l'erreur
     * dans le registre et on laisse Windows continuer. */
    (VOID)RtCodecInit(dev, GetCtx(dev));
    return STATUS_SUCCESS;
}

NTSTATUS RtEvtD0Exit(WDFDEVICE dev, WDF_POWER_DEVICE_STATE target)
{
    PDEVICE_CONTEXT ctx = GetCtx(dev);
    UNREFERENCED_PARAMETER(target);
    if (ctx->I2c) {
        /* Couper l'ampli puis remettre la puce a zero */
        (VOID)RtUpdate(ctx, RT5645_PWR_DIG1, 0x0301, 0x0000);
        (VOID)RtWrite(ctx, RT5645_SPK_VOL, 0xc8c8);
    }
    return STATUS_SUCCESS;
}

NTSTATUS RtEvtDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE dev;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(drv);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = RtEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = RtEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = RtEvtD0Entry;
    pnp.EvtDeviceD0Exit = RtEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    st = WdfDeviceCreate(&init, &attr, &dev);
    return st;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT obj, PUNICODE_STRING reg)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, RtEvtDeviceAdd);
    return WdfDriverCreate(obj, reg, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
