/* Empty cfattach struct stubs for SoC drivers we excluded from compilation
 * (because their files were stubbed in the wrapper). ioconf.o references
 * these via the autoconfig table; with stubs, the table entries point to
 * zero-filled structs that autoconfig walking treats as inactive drivers.
 *
 * Auto-extracted from linker errors. Do not hand-edit — regenerate from
 * the link error list.
 */

/* gcc with the kernel's -Werror -Wmissing-prototypes treats stub function
 * definitions without prior declarations as errors.  ncc does not enforce
 * this; the pragma below makes both compilers accept the file. */
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wold-style-definition"

/* cfattach has the layout (from sys/sys/device.h) but we don't need to
 * match exactly because nothing dereferences these — they're just present
 * so ioconf's pointer table can be filled. A 256-byte buffer is plenty. */
struct cfattach_stub_t { unsigned char filler[256]; };

struct cfattach_stub_t a64_acodec_ca;
struct cfattach_stub_t h3_codec_ca;
struct cfattach_stub_t rk_dwhdmi_ca;
struct cfattach_stub_t rk_eqos_ca;
struct cfattach_stub_t rk_fb_ca;
struct cfattach_stub_t rk_gmac_ca;
struct cfattach_stub_t rk_gpio_ca;
struct cfattach_stub_t rk_i2c_ca;
struct cfattach_stub_t rk_i2s_ca;
struct cfattach_stub_t rk_pwm_ca;
struct cfattach_stub_t rk_spi_ca;
struct cfattach_stub_t rk_tsadc_ca;
struct cfattach_stub_t rk_usb_ca;
struct cfattach_stub_t rk_usbphy_ca;
struct cfattach_stub_t rk_v1crypto_ca;
struct cfattach_stub_t rk_vop_ca;
struct cfattach_stub_t rk3588_cru_ca;
struct cfattach_stub_t rkemmcphy_ca;
struct cfattach_stub_t sun6i_dma_ca;
struct cfattach_stub_t sun6i_spi_ca;
struct cfattach_stub_t sun8i_codec_ca;
struct cfattach_stub_t sun8i_crypto_ca;
struct cfattach_stub_t sunxi_a64_ccu_ca;
struct cfattach_stub_t sunxi_a64_r_ccu_ca;
struct cfattach_stub_t sunxi_codec_ca;
struct cfattach_stub_t sunxi_de2bus_ca;
struct cfattach_stub_t sunxi_de2ccu_ca;
struct cfattach_stub_t sunxi_drm_ca;
struct cfattach_stub_t sunxi_dwhdmi_ca;
struct cfattach_stub_t sunxi_emac_ca;
struct cfattach_stub_t sunxi_fb_ca;
struct cfattach_stub_t sunxi_gates_ca;
struct cfattach_stub_t sunxi_gmac_ca;
struct cfattach_stub_t sunxi_gmacclk_ca;
struct cfattach_stub_t sunxi_gpio_ca;
struct cfattach_stub_t sunxi_h3_ccu_ca;
struct cfattach_stub_t sunxi_h3_r_ccu_ca;
struct cfattach_stub_t sunxi_h6_ccu_ca;
struct cfattach_stub_t sunxi_h6_r_ccu_ca;
struct cfattach_stub_t sunxi_hdmiphy_ca;
struct cfattach_stub_t sunxi_i2s_ca;
struct cfattach_stub_t sunxi_lcdc_ca;
struct cfattach_stub_t sunxi_mixer_ca;
struct cfattach_stub_t sunxi_mmc_ca;
struct cfattach_stub_t sunxi_musb_ca;
struct cfattach_stub_t sunxi_nmi_ca;
struct cfattach_stub_t sunxi_pwm_ca;
struct cfattach_stub_t sunxi_resets_ca;
struct cfattach_stub_t sunxi_rsb_ca;
struct cfattach_stub_t sunxi_rtc_ca;
struct cfattach_stub_t sunxi_sata_ca;
struct cfattach_stub_t sunxi_sid_ca;
struct cfattach_stub_t sunxi_sramc_ca;
struct cfattach_stub_t sunxi_thermal_ca;
struct cfattach_stub_t sunxi_twi_ca;
struct cfattach_stub_t sunxi_usb3phy_ca;
struct cfattach_stub_t sunxi_usbphy_ca;
struct cfattach_stub_t sunxi_wdt_ca;
struct cfattach_stub_t tegra_ahcisata_ca;
struct cfattach_stub_t tegra_apbdma_ca;
struct cfattach_stub_t tegra_cec_ca;
struct cfattach_stub_t tegra_com_ca;
struct cfattach_stub_t tegra_ehci_ca;
struct cfattach_stub_t tegra_fuse_ca;
struct cfattach_stub_t tegra_gpio_ca;
struct cfattach_stub_t tegra_hdaudio_ca;
struct cfattach_stub_t tegra_i2c_ca;
struct cfattach_stub_t tegra_lic_ca;
struct cfattach_stub_t tegra_mc_ca;
struct cfattach_stub_t tegra_pcie_ca;
struct cfattach_stub_t tegra_pinmux_ca;
struct cfattach_stub_t tegra_pmc_ca;
struct cfattach_stub_t tegra_rtc_ca;
struct cfattach_stub_t tegra_sdhc_ca;
struct cfattach_stub_t tegra_soctherm_ca;
struct cfattach_stub_t tegra_timer_ca;
struct cfattach_stub_t tegra_usbphy_ca;
struct cfattach_stub_t tegra_xusb_ca;
struct cfattach_stub_t tegra210_car_ca;
struct cfattach_stub_t tegra210_xusbpad_ca;
struct cfattach_stub_t tegra210xphy_ca;
/* devnullop, devenodev, ttyvnullop, ttyvenodev, ucas_int, ufetch_int,
 * ufetch_long, ustore_char are provided naturally by kern_stub.o and
 * subr_copy.o once they rebuilt against ncc with file-scope __asm support. */
/* These are FUNCTIONS in the kernel, not data. Empty bodies — they'll
 * be called but do nothing. (Earlier `int X;` declarations made them 4-byte
 * data symbols; calling them as functions hit the int data and crashed.) */
void spldebug_start(void) {}
void spldebug_stop(void) {}
void kobj_renamespace(void) {}
int pci_bus_devorder(void) { return 0; }
/* From the MD netbsd32_syscall.c which we stubbed (its file was sub'd to
 * empty so the MI version wins). */
void netbsd32_syscall_intern(void) {}

/* From kern_sdt.c which we stubbed. */
void sdt_probe_func(void) {}
struct cfattach_stub_t sdt_provider_sdt;

/* Empty link sets cause R_AARCH64_ADR_PREL_PG_HI21 relocation overflow:
 * with no entries the section's __start_/__stop_ markers are placed by
 * the linker at addresses that differ from the kernel's by more than 4 GB,
 * which ADRP cannot reach. Force one dummy entry in each affected set so
 * the section gets allocated at a sane address adjacent to the kernel. */
__attribute__((section("link_set_sdt_probes_set"), used))
static void *const __link_set_sdt_dummy = 0;
__attribute__((section("link_set_arm_cpu_methods"), used))
static void *const __link_set_armcpu_dummy = 0;
__attribute__((section("link_set_fdt_opps"), used))
static void *const __link_set_fdtopps_dummy = 0;
/* Explicitly initialized so ncc doesn't skip them as tentative __-prefixed
 * system variables (see codegen_arm64.c:emit_data). */
int __drm_debug = 0;
int __drm_printfn_seq_file = 0;
int __drm_puts_seq_file = 0;
/* aes_neon public API + per-block helpers. This file is substituted in
 * place of aes_neon_subr.c (which uses NEON intrinsics). aes_neon_impl
 * is the implementation table, defined separately in aes_neon_impl.c
 * (which we let compile normally — it's pure C). */
struct aesenc;
struct aesdec;
typedef unsigned char neon_uint8_t;
typedef unsigned long neon_size_t;
void aes_neon_setenckey(struct aesenc *e, const neon_uint8_t *k, unsigned r) { (void)e; (void)k; (void)r; }
void aes_neon_setdeckey(struct aesdec *d, const neon_uint8_t *k, unsigned r) { (void)d; (void)k; (void)r; }
void aes_neon_enc(const struct aesenc *e, const neon_uint8_t *in, neon_uint8_t *out, unsigned r) { (void)e; (void)in; (void)out; (void)r; }
void aes_neon_dec(const struct aesdec *d, const neon_uint8_t *in, neon_uint8_t *out, unsigned r) { (void)d; (void)in; (void)out; (void)r; }
void aes_neon_cbc_enc(const struct aesenc *e, const neon_uint8_t *in, neon_uint8_t *out, neon_size_t n, neon_uint8_t *iv, unsigned r) { (void)e; (void)in; (void)out; (void)n; (void)iv; (void)r; }
void aes_neon_cbc_dec(const struct aesdec *d, const neon_uint8_t *in, neon_uint8_t *out, neon_size_t n, neon_uint8_t *iv, unsigned r) { (void)d; (void)in; (void)out; (void)n; (void)iv; (void)r; }
void aes_neon_xts_enc(const struct aesenc *e, const neon_uint8_t *in, neon_uint8_t *out, neon_size_t n, neon_uint8_t *tw, unsigned r) { (void)e; (void)in; (void)out; (void)n; (void)tw; (void)r; }
void aes_neon_xts_dec(const struct aesdec *d, const neon_uint8_t *in, neon_uint8_t *out, neon_size_t n, neon_uint8_t *tw, unsigned r) { (void)d; (void)in; (void)out; (void)n; (void)tw; (void)r; }
void aes_neon_cbcmac_update1(const struct aesenc *e, const neon_uint8_t *in, neon_size_t n, neon_uint8_t *auth, unsigned r) { (void)e; (void)in; (void)n; (void)auth; (void)r; }
void aes_neon_ccm_enc1(const struct aesenc *e, const neon_uint8_t *in, neon_uint8_t *out, neon_size_t n, neon_uint8_t *authctr, unsigned r) { (void)e; (void)in; (void)out; (void)n; (void)authctr; (void)r; }
void aes_neon_ccm_dec1(const struct aesenc *e, const neon_uint8_t *in, neon_uint8_t *out, neon_size_t n, neon_uint8_t *authctr, unsigned r) { (void)e; (void)in; (void)out; (void)n; (void)authctr; (void)r; }
int aes_neon_selftest(void) { return 0; }
void aes_neon_dec1(void) {}
void aes_neon_dec2(void) {}
void aes_neon_enc1(void) {}
void aes_neon_enc2(void) {}
/* AGP functions — called by drm but our drm config doesn't actually
 * exercise AGP on QEMU virt. Empty fn bodies are safe. */
int agp_acquire(void *p) { (void)p; return 0; }
int agp_alloc_memory(void *p, int t, int s) { (void)p; (void)t; (void)s; return 0; }
int agp_bind_memory(void *p, void *m, long o) { (void)p; (void)m; (void)o; return 0; }
void *agp_find_device(int u) { (void)u; return 0; }
void agp_free_memory(void *p, void *m) { (void)p; (void)m; }
int agp_get_info(void *p, void *i) { (void)p; (void)i; return 0; }
void agp_release(void *p) { (void)p; }
int agp_unbind_memory(void *p, void *m) { (void)p; (void)m; return 0; }
/* ucas_int / ufetch_int / ufetch_long / ustore_char come from subr_copy.o now. */
/* Additional cfattach stubs for SoC drivers excluded from compilation. */

struct cfattach_stub_t apple_dart_ca;
struct cfattach_stub_t apple_iic_ca;
struct cfattach_stub_t apple_intc_ca;
struct cfattach_stub_t apple_mbox_ca;
struct cfattach_stub_t apple_nvme_ca;
struct cfattach_stub_t apple_pcie_ca;
struct cfattach_stub_t apple_pinctrl_ca;
struct cfattach_stub_t apple_pmgr_ca;
struct cfattach_stub_t apple_rtkitsmc_ca;
struct cfattach_stub_t apple_wdog_ca;
struct cfattach_stub_t bcmaux_fdt_ca;
struct cfattach_stub_t bcmcom_acpi_ca;
struct cfattach_stub_t bcmcom_ca;
struct cfattach_stub_t bcmcprman_fdt_ca;
struct cfattach_stub_t bcmdmac_fdt_ca;
struct cfattach_stub_t bcmdwctwo_ca;
struct cfattach_stub_t bcmemmc_acpi_ca;
struct cfattach_stub_t bcmemmc_ca;
struct cfattach_stub_t bcmemmc2_acpi_ca;
struct cfattach_stub_t bcmgenfb_ca;
struct cfattach_stub_t bcmgpio_ca;
struct cfattach_stub_t bcmicu_ca;
struct cfattach_stub_t bcmmbox_acpi_ca;
struct cfattach_stub_t bcmmbox_fdt_ca;
struct cfattach_stub_t bcmpmwdog_fdt_ca;
struct cfattach_stub_t bcmrng_fdt_ca;
struct cfattach_stub_t bcmsdhost_ca;
struct cfattach_stub_t bcmspi_ca;
struct cfattach_stub_t bsciic_acpi_ca;
struct cfattach_stub_t bsciic_fdt_ca;
struct cfattach_stub_t exynos_dwcmmc_ca;
struct cfattach_stub_t exynos_ehci_ca;
struct cfattach_stub_t exynos_ohci_ca;
struct cfattach_stub_t exynos_uart_ca;
struct cfattach_stub_t gxlphy_ca;
struct cfattach_stub_t imxgpio_ca;
struct cfattach_stub_t imxi2c_ca;
struct cfattach_stub_t meson_dwmac_ca;
struct cfattach_stub_t meson_pinctrl_ca;
struct cfattach_stub_t meson_pwm_ca;
struct cfattach_stub_t meson_resets_ca;
struct cfattach_stub_t meson_rng_ca;
struct cfattach_stub_t meson_thermal_ca;
struct cfattach_stub_t meson_uart_ca;
struct cfattach_stub_t meson_usbctrl_ca;
struct cfattach_stub_t meson_usbphy_ca;
struct cfattach_stub_t mesong12_aoclkc_ca;
struct cfattach_stub_t mesong12_clkc_ca;
struct cfattach_stub_t mesong12_usb2phy_ca;
struct cfattach_stub_t mesong12_usb3pciephy_ca;
struct cfattach_stub_t mesongx_mmc_ca;
struct cfattach_stub_t mesongx_wdt_ca;
struct cfattach_stub_t mesongxbb_aoclkc_ca;
struct cfattach_stub_t mesongxbb_clkc_ca;
struct cfattach_stub_t mesongxl_usb2phy_ca;
struct cfattach_stub_t mesongxl_usb3phy_ca;
struct cfattach_stub_t rk_anxdp_ca;
struct cfattach_stub_t rk_drm_ca;
struct cfattach_stub_t rk3328_cru_ca;
struct cfattach_stub_t rk3328_iomux_ca;
struct cfattach_stub_t rk3399_cru_ca;
struct cfattach_stub_t rk3399_iomux_ca;
struct cfattach_stub_t rk3399_pmucru_ca;
struct cfattach_stub_t rk3588_iomux_ca;
struct cfattach_stub_t rkpcie_ca;
struct cfattach_stub_t rkpciephy_ca;
struct cfattach_stub_t vcaudio_ca;

/* Helper functions and pseudo-tables — empty bodies. */
void bcmmbox_request(void) {}
void bcmmbox_write(void) {}
void enet_attach_common(void) {}
void enet_intr(void) {}
void imxcom_cdevsw(void) {}
void imxgpio_attach_common(void) {}
void imxgpio_pin_ctl(void) {}
void imxgpio_pin_read(void) {}
void imxgpio_pin_write(void) {}
void imxi2c_attach_common(void) {}
void imxuart_attach_subr(void) {}
void imxuart_cnattach(void) {}
void imxuart_is_console(void) {}
void imxuart_set_frequency(void) {}
void imxuintr(void) {}

/* Stubs for symbols referenced by net80211 / aes_ccm headers even when
 * the actual net80211 sources aren't compiled in.  Marked WEAK via a
 * file-scope `__asm(".weak ...")` so that GENERIC64 (which DOES compile
 * net80211 and provides the real strong `ieee80211_cipher_none`) wins
 * the linker's strong/weak resolution and we don't multi-def.  For
 * MINIMAL_VIRT64 (no net80211), the weak stub is used. */
__asm(".weak ieee80211_cipher_none");
struct cfattach_stub_t ieee80211_cipher_none;
