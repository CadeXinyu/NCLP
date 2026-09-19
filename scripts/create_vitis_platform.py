from pathlib import Path

import vitis

WORKSPACE = "vitis_project"
PLATFORM_NAME = "NCLP_platform"
CONTROL_DOMAIN = "standalone_psu_cortexa53_0"
CONTROL_CPU = "psu_cortexa53_0"
NETWORK_DOMAIN = "standalone_psu_cortexa53_1"
NETWORK_CPU = "psu_cortexa53_1"
XSA_PATH = "./vivado_project/NCLP_wrapper.xsa"


def configure_lwip(domain):
    """Configure the shared PS-GEM/lwIP contract for either A53 domain."""
    configured_libs = {entry["name"] for entry in domain.get_libs()}
    if "lwip220" not in configured_libs:
        domain.set_lib("lwip220")
    domain.set_config("lib", "lwip220_api_mode", "RAW_API", "lwip220")
    domain.set_config("lib", "lwip220_dhcp", "false", "lwip220")
    domain.set_config("lib", "lwip220_ipv6_enable", "false", "lwip220")
    domain.set_config("lib", "lwip220_pbuf_pool_size", "2048", "lwip220")
    # Let the TI PHY path program the KR260 J10C RGMII clock delays and
    # negotiate the actual link speed.  Forcing 1000 Mb/s skips that
    # board-critical tuning.
    domain.set_config(
        "lib",
        "lwip220_temac_phy_link_speed",
        "CONFIG_LINKSPEED_AUTODETECT",
        "lwip220",
    )

client = vitis.create_client()
client.set_workspace(path=WORKSPACE)

advanced_options = client.create_advanced_options_dict(dt_overlay="0")

try:
    platform = client.get_component(name=PLATFORM_NAME)
except Exception:
    platform = None

if platform is None:
    platform = client.create_platform_component(
        name=PLATFORM_NAME, hw_design=XSA_PATH, os="standalone",
        cpu=CONTROL_CPU, domain_name=CONTROL_DOMAIN, generate_dtb=False,
        advanced_options=advanced_options, compiler="gcc",
    )
else:
    platform.update_hw(hw_design=XSA_PATH)
    system_dts = Path(f"{WORKSPACE}/{PLATFORM_NAME}/hw/sdt/system-top.dts")
    if not system_dts.is_file():
        raise RuntimeError(f"hardware update did not generate {system_dts}; inspect the SDT tool error")
    print(f"Updating existing platform {PLATFORM_NAME}.")

# A53-0 owns PL/headstage control.  A53-1 is intentionally dedicated to the
# PS GEM/lwIP transport so network polling cannot stall during calculations.
try:
    network_domain = platform.get_domain(name=NETWORK_DOMAIN)
except Exception:
    network_domain = platform.add_domain(
        cpu=NETWORK_CPU,
        os="standalone",
        name=NETWORK_DOMAIN,
        display_name=NETWORK_DOMAIN,
        generate_dtb=False,
        dt_overlay=False,
        architecture="64-bit",
        compiler="gcc",
    )

# Production uses A53-1 for networking, while the single-core sanity image
# intentionally runs the same GEM1 diagnostics on A53-0.  Keep both BSPs able
# to compile the shared PS Ethernet implementation.
configure_lwip(platform.get_domain(name=CONTROL_DOMAIN))
configure_lwip(network_domain)

platform.build()

vitis.dispose()

platform_output = Path(
    f"{WORKSPACE}/{PLATFORM_NAME}/export/{PLATFORM_NAME}/{PLATFORM_NAME}.xpfm"
)
if not platform_output.is_file() or platform_output.stat().st_size == 0:
    raise RuntimeError(f"Vitis build did not produce: {platform_output}")

print(f"Built platform {PLATFORM_NAME} in {WORKSPACE}.")
print(f"Platform output: {platform_output}")
print(f"Control domain: {CONTROL_DOMAIN} on {CONTROL_CPU}")
print(f"Network domain: {NETWORK_DOMAIN} on {NETWORK_CPU}")
