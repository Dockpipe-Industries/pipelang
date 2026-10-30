"""Discover the native toolchain closure with installed compilers; no downloads."""
import argparse
import json
import re
import shlex
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--core-only', action='store_true', help='Discover C++ inputs without optional Qt pilot inputs')
    arguments = parser.parse_args()
    files = set()

    def add(path):
        selected = Path(path).resolve()
        if selected.is_file():
            files.add(selected)

    def command(invocation, **options):
        return subprocess.check_output(invocation, text=True, timeout=20, **options).strip()

    probe = '\n'.join('#include <' + header + '>' for header in (
        'fstream', 'iostream', 'cstring', 'limits', 'vector', 'stdexcept')) + '\n'
    invocation = ['/usr/bin/g++', '-std=c++17', '-fPIC', '-M', '-x', 'c++', '-']
    programs = ['/usr/bin/g++', '/usr/bin/as', '/usr/bin/ld', '/usr/bin/make',
                '/usr/bin/cmake', '/usr/bin/time', '/usr/bin/readelf', '/usr/bin/ldd']
    qt_version = None
    cmake_roots = [Path('/usr/share/cmake-3.22/Modules')]
    if not arguments.core_only:
        qmake = '/usr/lib/qt6/bin/qmake6'
        qt = Path(command([qmake, '-query', 'QT_INSTALL_HEADERS']))
        qt_version = command([qmake, '-query', 'QT_VERSION'])
        for header in ('QApplication', 'QLabel', 'QLineEdit', 'QPushButton', 'QVBoxLayout',
                       'QPointer', 'QTimer', 'QFile', 'QJsonDocument', 'QJsonObject', 'QPixmap', 'QThread'):
            probe += '#include <' + header + '>\n'
        invocation += ['-I' + str(qt)]
        invocation += ['-I' + str(qt / name) for name in ('QtCore', 'QtGui', 'QtWidgets')]
        programs += ['/usr/lib/qt6/libexec/moc', qmake,
                     '/usr/lib/x86_64-linux-gnu/libQt6Core.so',
                     '/usr/lib/x86_64-linux-gnu/libQt6Gui.so',
                     '/usr/lib/x86_64-linux-gnu/libQt6Widgets.so',
                     '/usr/lib/x86_64-linux-gnu/qt6/plugins/platforms/libqxcb.so']
        cmake_roots += list(Path('/usr/lib/x86_64-linux-gnu/cmake').glob('Qt6*'))
    dependencies = command(invocation, input=probe).replace('\\\n', ' ')
    for path in shlex.split(dependencies.split(':', 1)[1]):
        add(path)
    for name in ('cc1plus', 'collect2'):
        programs.append(command(['/usr/bin/g++', '-print-prog-name=' + name]))
    for name in ('crt1.o', 'Scrt1.o', 'crti.o', 'crtn.o', 'crtbegin.o', 'crtbeginS.o',
                 'crtend.o', 'crtendS.o', 'libstdc++.so', 'libgcc.a', 'libgcc_s.so', 'libm.so', 'libc.so'):
        add(command(['/usr/bin/g++', '-print-file-name=' + name]))
    for path in programs:
        add(path)
        result = subprocess.run(['/usr/bin/ldd', path], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=20)
        for dependency in re.findall(r'(/[^\s()]+)', result.stdout):
            add(dependency)
    for root in cmake_roots:
        for path in root.rglob('*'):
            if path.is_file():
                add(path)
    for name in ('libc.so.6', 'libc_nonshared.a', 'libm.so.6', 'libmvec.so.1',
                 'libmvec_nonshared.a', 'libpthread_nonshared.a'):
        add(Path('/usr/lib/x86_64-linux-gnu') / name)
    arguments.output.write_text(json.dumps({
        'files': sorted(map(str, files)),
        'gxx': command(['/usr/bin/g++', '--version']).splitlines()[0],
        'qt': qt_version,
        'core_only': arguments.core_only,
        'probe': probe,
        'command': invocation,
    }, indent=2))
    print('native dependency files:', len(files))


if __name__ == '__main__':
    main()
