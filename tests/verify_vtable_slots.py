# Checks every COM vtable slot the adapters hook against the compiler's own view of the mingw-w64 headers.
# Run: python3 tests/verify_vtable_slots.py   (needs i686/x86_64 mingw-w64 g++)
import subprocess, re, os, sys
import tempfile; os.chdir(tempfile.mkdtemp())
for f in os.listdir('.'):
    if f.endswith(('.cpp','.s')): os.remove(f)
# (name, header, C++ expression, expected slot)
groups = {
 'dxgi': ('#include <dxgi1_2.h>\n#include <d3d11.h>\n', [
    ('CONTROL IUnknown::AddRef','&IUnknown::AddRef',1),
    ('CONTROL IUnknown::Release','&IUnknown::Release',2),
    ('IDXGISwapChain::Present','&IDXGISwapChain::Present',8),
    ('IDXGISwapChain1::Present1','&IDXGISwapChain1::Present1',22),
    ('IDXGIFactory::CreateSwapChain','&IDXGIFactory::CreateSwapChain',10),
    ('IDXGIFactory2::CreateSwapChainForHwnd','&IDXGIFactory2::CreateSwapChainForHwnd',15),
    ('IDXGIFactory2::CreateSwapChainForCoreWindow','&IDXGIFactory2::CreateSwapChainForCoreWindow',16),
    ('IDXGIFactory2::CreateSwapChainForComposition','&IDXGIFactory2::CreateSwapChainForComposition',24)]),
 'd3d9': ('#include <d3d9.h>\n', [
    ('CONTROL IUnknown::Release','&IUnknown::Release',2),
    ('IDirect3DDevice9::Present','&IDirect3DDevice9::Present',17),
    ('IDirect3DSwapChain9::Present','&IDirect3DSwapChain9::Present',3),
    ('IDirect3DDevice9Ex::PresentEx','&IDirect3DDevice9Ex::PresentEx',121)]),
 'd3d8': ('#include <d3d8.h>\n', [
    ('CONTROL IUnknown::Release','&IUnknown::Release',2),
    ('IDirect3DDevice8::Present','&IDirect3DDevice8::Present',15)]),
}
bad = 0
for tc, ptr in (('x86_64',8),('i686',4)):
    for g,(inc,items) in groups.items():
        src = inc + ''.join(f'extern const auto pm_{i} = {expr};\n' for i,(n,expr,e) in enumerate(items))
        open(f'{g}.cpp','w').write(src)
        r = subprocess.run([f'{tc}-w64-mingw32-g++','-std=c++17','-O0','-S',f'{g}.cpp','-o',f'{g}_{tc}.s'],capture_output=True,text=True)
        if r.returncode != 0:
            print(f'{tc} {g}: COMPILE FAILED', r.stderr[:300]); bad += 1; continue
        asm = open(f'{g}_{tc}.s').read()
        for i,(n,expr,exp) in enumerate(items):
            m = re.search(r'\n_?pm_%d:\n(?:\s*\.[a-z0-9]+[^\n]*\n)*?\s*\.(?:quad|long)\s+(\d+)' % i, asm)
            if not m: print(f'{tc} {n}: could not find constant in assembly'); bad += 1; continue
            raw = int(m.group(1)); slot = (raw - 1)//ptr if raw & 1 else None
            ok = slot == exp
            if not ok: bad += 1
            print(f'{tc:6} {n:48} compiler says slot {slot!s:>4}, code uses {exp:>3}  {"OK" if ok else "*** MISMATCH ***"}')
print('\nRESULT:', 'ALL SLOTS VERIFIED' if bad == 0 else f'{bad} PROBLEM(S)')
sys.exit(1 if bad else 0)
