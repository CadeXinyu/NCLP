"""One source/build/packaging definition per Vitis application."""
from dataclasses import dataclass
import os
from pathlib import Path
import shutil

WORKSPACE = 'vitis_project'
PLATFORM_NAME = 'NCLP_platform'
CONTROL_DOMAIN = 'standalone_psu_cortexa53_0'
NETWORK_DOMAIN = 'standalone_psu_cortexa53_1'

@dataclass(frozen=True)
class Application:
    name: str
    domain: str
    source_root: str
    sources: tuple[str, ...]
    linker: str
    libraries: str
    optimization: str = '-O0'
    definitions: str = ''
    link_flags: str = ''
    common: tuple[str, ...] = ('nclp_pl_registers.h', 'nclp_wire.h')


def enabled(name):
    return int(os.environ.get(name, '0').lower() not in ('', '0', 'false'))


def applications(kind):
    if kind == 'sanity':
        return (Application('sanity_check', CONTROL_DOMAIN, 'software/sanity_check/src',
                            ('main.c', 'ps_ethernet.c', 'ps_ethernet.h'),
                            'software/sanity_check/src/lscript.ld', 'lwip220;m',
                            common=('nclp_pl_registers.h', 'nclp_wire.h', 'board/fan_control.c', 'board/fan_control.h')),)
    debug = enabled('NCLP_DEBUG_MODE')
    uart = enabled('NCLP_NETWORK_UART')
    log = int(os.environ.get('NCLP_LOG_LEVEL', '0'))
    control_sources = ('app/main.c', 'drivers/led_control.c', 'drivers/led_control.h',
                       'drivers/stim_control.c', 'drivers/stim_control.h',
                       'drivers/compute_control.c', 'drivers/compute_control.h',
                       'drivers/ttl_router.c', 'drivers/ttl_router.h',
                       'drivers/dac_preset.c', 'drivers/dac_preset.h',
                       'drivers/ripple_detector_control.c', 'drivers/ripple_detector_control.h',
                       'drivers/ripple_filter_design.c', 'drivers/ripple_filter_design.h',
                       'drivers/ripple_detector_service.c', 'drivers/ripple_detector_service.h',
                       'drivers/intan_sync_control.c', 'drivers/intan_sync_control.h',
                       'protocol/output_commands.c', 'protocol/output_commands.h',
                       'protocol/nclp_main.h', 'protocol/nclp_results.h', 'ipc/nclp_shared.h',
                       'ipc/result_store_control.c', 'ipc/result_store_control.h',
                       'diagnostics/debug_autorun.c', 'diagnostics/debug_autorun.h')
    headstage_sources = tuple(str(p.relative_to('software/main/src')) for p in
                              sorted(Path('software/main/src/headstage').glob('*'))
                              if p.suffix in ('.c', '.h'))
    return (
        Application('main_debug' if debug else 'main', CONTROL_DOMAIN, 'software/main/src',
                    control_sources + headstage_sources, 'software/main/linker/control.ld', 'm',
                    definitions=f'NCLP_DEBUG_MODE={debug};NCLP_LOG_LEVEL={log}',
                    common=('nclp_pl_registers.h', 'nclp_wire.h', 'board/fan_control.c', 'board/fan_control.h')),
        Application('main_network', NETWORK_DOMAIN, 'software/main/src',
                    ('app/network_main.c', 'network/ps_ethernet.c', 'network/ps_ethernet.h',
                     'protocol/nclp_text_command.c', 'protocol/nclp_text_command.h',
                     'protocol/nclp_command_service.c', 'protocol/nclp_command_service.h',
                     'network/sfp_control.c', 'network/sfp_control.h',
                     'network/nclp_udp_packetizer.c', 'network/nclp_udp_packetizer.h',
                     'network/network_uart.c', 'ipc/shared_network.c',
                     'ipc/result_store_network.c', 'ipc/result_store_network.h',
                     'protocol/nclp_main.h', 'ipc/nclp_shared.h', 'protocol/nclp_results.h'),
                    'software/main/linker/network.ld', 'lwip220', '-O2',
                    f'NCLP_NETWORK_UART={uart}', '' if uart else '-Wl,--wrap=outbyte'))


def configure(app, spec):
    values = {'USER_COMPILE_OPTIMIZATION_LEVEL': spec.optimization,
              'USER_COMPILE_DEBUG_LEVEL': '-g3',
              'USER_COMPILE_DEFINITIONS': spec.definitions,
              'USER_LINK_LIBRARIES': spec.libraries,
              'USER_LINK_OTHER_FLAGS': spec.link_flags,
              'USER_LINKER_SCRIPT': str(Path(spec.linker).resolve())}
    for key, value in values.items():
        app.set_app_config(key, value)


def sync_debug_bitstream(name):
    # Vitis does not refresh existing application debug copies after a platform
    # rebuild. Keep JTAG launches on the same hardware image as BOOT.bin.
    source = Path(f'{WORKSPACE}/{PLATFORM_NAME}/export/{PLATFORM_NAME}/hw/sdt/NCLP_wrapper.bit')
    destination = Path(f'{WORKSPACE}/{name}/_ide/bitstream/NCLP_wrapper.bit')
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def build_apps(kind, recreate):
    import vitis
    specs = applications(kind)
    client = vitis.create_client()
    client.set_workspace(path=WORKSPACE)
    platform_repo = str(Path(f'{WORKSPACE}/{PLATFORM_NAME}/export/{PLATFORM_NAME}').resolve())
    if recreate:
        client.add_platform_repos(platform=platform_repo)
        client.rescan_platform_repos(platform=platform_repo)
    try:
        for spec in specs:
            if recreate:
                # Lookup failure means absent. Deletion failure must propagate.
                try:
                    existing = client.get_component(name=spec.name)
                except Exception:
                    existing = None
                if existing is not None:
                    client.delete_component(name=spec.name)
                app = client.create_app_component(name=spec.name,
                    platform=f'{platform_repo}/{PLATFORM_NAME}.xpfm', domain=spec.domain,
                    template='empty_application')
                app.import_files(from_loc=spec.source_root, files=list(spec.sources), is_skip_copy_sources=True)
                app.import_files(from_loc='software/common', files=list(spec.common), is_skip_copy_sources=True)
            else:
                app = client.get_component(name=spec.name)
            configure(app, spec)
            app.build()
            elf = Path(f'{WORKSPACE}/{spec.name}/build/{spec.name}.elf')
            if not elf.is_file() or not elf.stat().st_size:
                raise RuntimeError(f'Vitis did not produce {elf}')
            sync_debug_bitstream(spec.name)
        owner = specs[0].name
        lines = ['the_ROM_image:', '{',
                 '    [bootloader, destination_cpu = a53-0] ../NCLP_platform/zynqmp_fsbl/build/fsbl.elf',
                 '    [destination_device = pl] ../NCLP_platform/export/NCLP_platform/hw/sdt/NCLP_wrapper.bit',
                 f'    [destination_cpu = a53-0, exception_level = el-3] build/{owner}.elf']
        if kind == 'main':
            lines += ['    [destination_cpu = a53-1, exception_level = el-3] ../main_network/build/main_network.elf']
        lines += ['}', '']
        Path(f'{WORKSPACE}/{owner}/boot.bif').write_text('\n'.join(lines))
    finally:
        vitis.dispose()
    print('PASS: built ' + ', '.join(spec.name for spec in specs))
