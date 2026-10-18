"""Extract CoD4 (MW1, PC) guns for the shooter joke mode.

    python tools/cod4_extract.py [--mw1 mw1] [--out game/mods/guns] [weapon ...]

With no weapon names it lists the multiplayer weapons it can extract. For
each weapon (e.g. ak47_mp) it writes <out>/<weapon>/:
  weapon.ini  every key of the raw weapon file (main/iw_11.iwd), key=value,
              plus x_flash, the muzzle in the model's space
  gun.obj/mtl the third-person worldModel's LOD0 from zone/english/common_mp.ff
  *.wav       fire, dry, raise, clipout, clipin, chamber sounds, and for shell-by-shell
              guns start, loop, end, rechamber (localized_common_mp.ff)
  *.dds       each surface's colour map, from the main/iw_*.iwd images
and into --fx (game/mods/cod4/fx/) the muzzle flash, tracer and bullet impact
sprites and the impact sounds (impact.wav, flesh.wav).

Everything read and written is the user's own game data: the output lives
under game/, which is gitignored. Format notes are in docs/MW1_NOTES.md.
"""
import argparse, glob, os, re, struct, sys, wave, zipfile, zlib

FF = 0xFFFFFFFF
XMODEL_SIZE, XSURFACE_SIZE, MATERIAL_SIZE = 0xDC, 0x38, 0x50
BLOCK_VERTEX, BLOCK_INDEX = 7, 8


def load_zone(path):
    b = open(path, 'rb').read()
    if b[:8] != b'IWffu100' or struct.unpack_from('<I', b, 8)[0] != 5:
        sys.exit(f'{path}: not a CoD4 PC fastfile')
    return zlib.decompress(b[12:])


def cstr(z, o):
    e = z.index(b'\0', o)
    return z[o:e].decode('latin1'), e + 1


def ref_block(p):
    """A zone pointer that isn't -1 is ((block << 28) | offset) + 1."""
    p -= 1
    return p >> 28, p & 0x0FFFFFFF


def zone_strings(z):
    """The zone's script strings (bone names are indices into these)."""
    o = 4 * 11                          # size, externalSize, 9 block sizes
    n, = struct.unpack_from('<i', z, o)
    o += 16
    ptrs = struct.unpack_from(f'<{n}I', z, o)
    o += 4 * n
    out = []
    for p in ptrs:
        if p != FF:
            out.append(None)
            continue
        s, o = cstr(z, o)
        out.append(s)
    return out


class Zone:
    """Inline XModels of a zone, walked in stream order so the vertex and
    index block offsets (which back-references point at) can be tracked."""

    def __init__(self, z):
        self.z = z
        self.models = {}            # name -> model dict
        self.blocks = {BLOCK_VERTEX: {}, BLOCK_INDEX: {}}   # offset -> (stream pos, count)
        self.counts = {}            # (block, count) -> sorted [(offset, stream pos)]
        self.strings = zone_strings(z)
        cur = {BLOCK_VERTEX: 0, BLOCK_INDEX: 0}
        for s in self._scan():
            try:
                m = self._walk(s)
            except (ValueError, struct.error, IndexError):
                m = None
            if not m:
                continue
            for surf in m['surfs']:
                for blk, key, size in ((BLOCK_VERTEX, 'verts', 32 * surf['vc']),
                                       (BLOCK_INDEX, 'tris', 6 * surf['tc'])):
                    if surf[key + '_at'] is not None:
                        off = cur[blk] = (cur[blk] + 15) & ~15
                        self.blocks[blk][off] = surf[key + '_at']
                        n = surf['vc'] if key == 'verts' else surf['tc']
                        self.counts.setdefault((blk, n), []).append((off, surf[key + '_at']))
                        surf[key + '_off'] = off
                        cur[blk] += size
            self.models.setdefault(m['name'].lower(), m)

    def _scan(self):
        """Offsets of inline XModel structs: name pointer -1, plausible counts,
        all eight array pointers -1, and the inline name right after."""
        z = self.z
        pat = re.compile(rb'\xff\xff\xff\xff[\x01-\x80][\x01-\x80][\x01-\x40][\x00-\x03]'
                         rb'(?:\xff\xff\xff\xff|\x00\x00\x00\x00){6}\xff\xff\xff\xff\xff\xff\xff\xff', re.S)
        for m in pat.finditer(z):
            s = m.start()
            if re.match(rb'[a-z0-9_@/.\-]{3,63}\x00', z[s + XMODEL_SIZE:s + XMODEL_SIZE + 64], re.I):
                yield s

    def _walk(self, s):
        z = self.z
        nb, nr, ns = z[s + 4], z[s + 5], z[s + 6]
        if nr > nb:
            return None
        name, p = cstr(z, s + XMODEL_SIZE)
        n = nb - nr
        # boneNames, parentList, quats, trans, partClassification, baseMat:
        # each is inline when its pointer is -1 and absent when it is null
        ptrs = struct.unpack_from('<6I', z, s + 8)
        bones = [self.strings[i] if i < len(self.strings) else None
                 for i in struct.unpack_from(f'<{nb}H', z, p)] if ptrs[0] == FF else []
        base = {}                     # bone -> model-space position (baseMat: quat, then pos)
        for k, (ptr, size) in enumerate(zip(ptrs, (2 * nb, n, 8 * n, 16 * n, nb, 32 * nb))):
            if ptr == FF:
                if k == 5 and bones:
                    base = {bones[i]: struct.unpack_from('<3f', z, p + 32 * i + 16) for i in range(nb)}
                p += size
        surf0, p = p, p + XSURFACE_SIZE * ns
        surfs = []
        for i in range(ns):
            b = surf0 + XSURFACE_SIZE * i
            vc, tc = struct.unpack_from('<HH', z, b + 2)
            tri, = struct.unpack_from('<I', z, b + 0xC)
            bc = struct.unpack_from('<4h', z, b + 0x10)
            blend, verts, nvl, vl = struct.unpack_from('<IIII', z, b + 0x18)
            if not 0 < vc < 65536 or not 0 < tc < 65536 or nvl > 64:
                return None
            d = dict(vc=vc, tc=tc, tri=tri, vref=verts, verts_at=None, tris_at=None,
                     verts_off=None, tris_off=None, blend=sum(bc) > 0, bidx=None)
            if blend == FF:
                p += 2 * (bc[0] + 3 * bc[1] + 5 * bc[2] + 7 * bc[3])
            if verts == FF:
                d['verts_at'] = p
                p += 32 * vc
            if vl == FF:
                lists = [struct.unpack_from('<HHHHI', z, p + 12 * k) for k in range(nvl)]
                p += 12 * nvl
                # XRigidVertList.boneOffset is the bone index * sizeof(DObjSkelMat)
                d['bidx'] = [l[0] // 64 for l in lists]
                for l in lists:
                    if l[4] == FF:      # XSurfaceCollisionTree, nodes, leafs
                        nn, = struct.unpack_from('<I', z, p + 0x18)
                        nl, = struct.unpack_from('<I', z, p + 0x20)
                        p += 0x28 + 16 * nn + 2 * nl
            if tri == FF:
                d['tris_at'] = p
                p += 6 * tc
            if p > len(z):
                return None
            surfs.append(d)
        mats = list(struct.unpack_from(f'<{ns}I', z, p))
        names = self._inline_materials(p + 4 * ns, mats.count(FF))
        it = iter(names)
        for surf, h in zip(surfs, mats):
            surf['mat'] = next(it, None) if h == FF else None
            surf['mref'] = h
        return dict(name=name, surfs=surfs, bones=bones, base=base)

    def _inline_materials(self, p, count):
        """(material name, colour map image name) of the next `count` inline
        Materials: each is a 0x50 struct (name pointer -1) then its name."""
        z, out = self.z, []
        # model materials are named mc/...; a material's techniques, images
        # and constants follow it, so the next one starts within a few KB
        pat = re.compile(rb'\xff\xff\xff\xff.{76}(mc/[a-z0-9_\-]{3,60})\x00', re.S | re.I)
        while len(out) < count:
            m = pat.search(z, p, p + 0x4000)
            if not m:
                break
            mat, p = m.group(1).decode(), m.end()
            nxt = pat.search(z, p, p + 0x4000)
            region = z[p:nxt.start() if nxt else p + 0x2000]
            imgs = [i.decode() for i in re.findall(rb'\xff\xff\xff\xff([a-z0-9_\-~&$]{3,63})\x00', region, re.I)]
            out.append((mat, colour_map(mat, imgs)))
        return out

    def resolve(self, surf, key):
        """Stream position of a surface's verts ('verts') or indices ('tris')."""
        if surf[key + '_at'] is not None:
            return surf[key + '_at']
        blk, off = ref_block(surf['vref'] if key == 'verts' else surf['tri'])
        if off in self.blocks.get(blk, {}):
            return self.blocks[blk][off]
        # ponytail: our block offsets drift (assets other than XModels also
        # write these blocks), so take the nearest earlier array with the same
        # element count. Track every asset type if this ever picks wrong.
        n = surf['vc'] if key == 'verts' else surf['tc']
        best = [pos for o, pos in self.counts.get((blk, n), []) if o <= off]
        return best[-1] if best else None

    def bones(self, model, surf):
        """Bone names a rigid surface is attached to. A surface that borrows
        its vertex lists takes the bone indices of the surface it borrows
        from (the same stream data), named by this model's skeleton."""
        bidx = surf['bidx']
        if bidx is None:
            pos = self.resolve(surf, 'tris')
            bidx = next((s['bidx'] for m in self.models.values() for s in m['surfs']
                         if s['tris_at'] == pos and s['bidx'] is not None), [])
        return {model['bones'][b] for b in bidx if b < len(model['bones'])}

    def material(self, model, i):
        """(material, image) of surface i, following back-references: a
        surface whose indices are borrowed from an earlier surface is assumed
        to share its material too."""
        surf = model['surfs'][i]
        if surf['mat']:
            return surf['mat']
        for key in ('tris', 'verts'):
            pos = self.resolve(surf, key)
            for m in self.models.values():
                for s in m['surfs']:
                    if s[key + '_at'] == pos and s['mat'] and s is not surf:
                        return s['mat']
        same = [s['mat'] for s in model['surfs'] if s['mat'] and s['mref'] == surf['mref']]
        return (same or [(None, None)])[0]


def colour_map(mat, imgs):
    plain = [i for i in imgs if not i.startswith('~')]
    for i in plain:
        if re.search(r'_(c|col|color)$', i):
            return i
    for i in plain:
        if not re.search(r'_(n|nml|nrm|s|spc|spec|g)$', i):
            return i
    return None


def gun_image(model, files):
    """The colour map most likely to be the gun body: an image named after
    the viewmodel (viewmodel_ak47_mp -> weapon_ak47_c / _col / plain).
    ponytail: name heuristic for surfaces whose material is a reference we
    can't follow; resolve Material back-references if a gun looks wrong."""
    full = re.sub(r'^viewmodel_|_mp$', '', model.lower())
    for pat in (r'_(c|col)$', r'^weapon_[a-z0-9]+$'):     # plain names are often HUD icons
        tok = full
        while tok:
            hit = sorted((n[7:-4] for n in files if n.startswith(f'images/weapon_{tok}')
                          and n.endswith('.iwi') and re.search(pat, n[7:-4])), key=len)
            if hit:
                return hit[0]
            tok = tok[:-1] if '_' not in tok else tok.rpartition('_')[0]
    return None


def iwi_to_dds(data):
    """IWI v6 (CoD4) to DDS: same DXT blocks, mip chain stored smallest first."""
    if data[:3] != b'IWi' or data[3] != 6:
        return None
    fmt, _, w, h = data[4], data[5], *struct.unpack_from('<HH', data, 6)
    four, bpb = {0x0B: (b'DXT1', 8), 0x0C: (b'DXT3', 16), 0x0D: (b'DXT5', 16)}.get(fmt, (None, 0))
    if not four:
        return None
    sizes, lw, lh = [], w, h
    while True:
        sizes.append(max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * bpb)
        if lw == 1 and lh == 1:
            break
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    body = data[28:]
    while sum(sizes) > len(body):       # images without a full chain
        sizes.pop()
    mips, o = [], len(body)
    for sz in sizes:                    # largest level is at the end
        mips.append(body[o - sz:o])
        o -= sz
    hdr = struct.pack('<4sIIIIIII44xII4s20xI16x', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000,
                      h, w, sizes[0], 0, len(sizes), 32, 0x4, four, 0x1000 | 0x400000 | 0x8)
    return hdr + b''.join(mips)


def unpack_unit(u):
    """PackedUnitVec: xyz bytes biased by 127, scaled by the w byte."""
    b = u.to_bytes(4, 'little')
    scale = (b[3] + 192.0) / 32385.0
    return tuple((c - 127.0) * scale for c in b[:3])


def half(x):
    return struct.unpack('<e', struct.pack('<H', x))[0]


def write_obj(z, zone, model, folder, images, hide, fallback):
    lines, mtl, base = ['# CoD4 viewmodel ' + model['name']], {}, 0
    for i, surf in enumerate(model['surfs']):
        bones = zone.bones(model, surf)
        if bones and bones <= hide:
            print(f'  surface {i}: hidden ({", ".join(sorted(bones))})')
            continue
        vp, tp = zone.resolve(surf, 'verts'), zone.resolve(surf, 'tris')
        if vp is None or tp is None or surf['blend']:
            print(f'  surface {i}: skipped ({"skinned" if surf["blend"] else "unresolved data"})')
            continue
        mat, img = zone.material(model, i)
        img = img if img and images(img) else fallback
        key = img or 'untextured'
        if img and key not in mtl:
            dds = images(img)
            if dds:
                open(os.path.join(folder, img + '.dds'), 'wb').write(dds)
            else:
                key = 'untextured'
        mtl[key] = key != 'untextured'
        lines.append(f'g surf{i}\nusemtl {key}')
        for k in range(surf['vc']):
            o = vp + 32 * k
            x, y, zz = struct.unpack_from('<3f', z, o)
            tc, nrm = struct.unpack_from('<II', z, o + 20)
            nx, ny, nz = unpack_unit(nrm)
            lines.append(f'v {x:.5f} {y:.5f} {zz:.5f}')
            lines.append(f'vt {half(tc >> 16):.5f} {1.0 - half(tc & 0xFFFF):.5f}')
            lines.append(f'vn {nx:.4f} {ny:.4f} {nz:.4f}')
        idx = struct.unpack_from(f'<{3 * surf["tc"]}H', z, tp)
        for t in range(surf['tc']):
            a, b, c = (base + idx[3 * t + j] + 1 for j in range(3))
            lines.append(f'f {a}/{a}/{a} {c}/{c}/{c} {b}/{b}/{b}')
        base += surf['vc']
        print(f'  surface {i}: {surf["vc"]} verts, {surf["tc"]} tris, {mat or "?"} -> {key}')
    open(os.path.join(folder, 'gun.obj'), 'w').write('mtllib gun.mtl\n' + '\n'.join(lines) + '\n')
    with open(os.path.join(folder, 'gun.mtl'), 'w') as f:
        for k, tex in mtl.items():
            f.write(f'newmtl {k}\nKd 1 1 1\n' + (f'map_Kd {k}.dds\n' if tex else ''))


LOADED_SOUND = re.compile(rb'\xff\xff\xff\xff\x01\x00\x00\x00(.{32})\xfe\xff\xff\xff([ -~]{3,120}?\.wav)\x00', re.S)
NEXT_ALIAS = re.compile(rb'\xff{8}[\x01-\x40]\x00\x00\x00[a-z0-9_]{3,80}\x00', re.I)


_sound_files = {}


def sound_by_file(z, alias):
    """A shared sound's LoadedSound found through its file name instead.
    ponytail: CoD4's naming (weap_<gun>_slst_* for player fire,
    wpfoly_<gun>_reload_<part>* for reload foley), the alias's gun token
    shortened until a file matches; resolve LoadedSound back-references
    (block offsets) if a gun ever gets the wrong sound."""
    if id(z) not in _sound_files:
        _sound_files[id(z)] = {m.group(2).decode().lower(): m for m in LOADED_SOUND.finditer(z)}
    files = _sound_files[id(z)]
    m = re.match(r'weap_(\w+?)_(fire|clipin|clipout|chamber|start|loop|end|rechamber|pump)', alias)
    if not m:
        return None
    tok, kind = m.groups()
    # shell-by-shell foley: wpfoly_m40a3_start_*, wpfoly_winchester_reload_lift_* (its start)
    kinds = {'start': 'start|lift', 'rechamber': 'rechamber|pump', 'pump': 'rechamber|pump'}.get(kind, kind)
    pat = (r'weapons/\w+/weap_%s\w*_slst' if kind == 'fire'
           else r'foley/wpfoly_%s\w*_(?:reload_)?(?:' + kinds + ')')
    while len(tok) >= 2:
        hit = sorted(n for n in files if re.match(pat % re.escape(tok), n))
        if hit:
            return files[hit[0]]
        tok = tok[:-1]
    return None


def write_wav(z, alias, path):
    """A sound alias's first inline PCM (localized_common_mp.ff) as a .wav.

    snd_alias_list_t: name -1, head -1, u32 count, the name; each alias's
    LoadedSound: name -1, {format 1 = PCM, data ptr, len, rate, bits, channels,
    samples, block, initial ptr}, data -2, the file name, then the PCM. A sound
    shared with an earlier alias is a back-reference, so it is not found here."""
    m = re.search(rb'\xff{8}(.{4})' + re.escape(alias.encode()) + rb'\x00', z, re.S)
    s = None
    if m:
        nxt = NEXT_ALIAS.search(z, m.end())
        s = LOADED_SOUND.search(z, m.end(), nxt.start() if nxt else len(z))
    if not s:
        s = sound_by_file(z, alias)
    if not s:
        return False
    _, ln, rate, bits, ch, _, _, _ = struct.unpack('<8I', s.group(1))
    if bits not in (8, 16) or not 1 <= ch <= 2:
        return False
    with wave.open(path, 'wb') as w:
        w.setnchannels(ch)
        w.setsampwidth(bits // 8)
        w.setframerate(rate)
        w.writeframes(z[s.end():s.end() + ln])
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mw1', default='mw1')
    ap.add_argument('--out', default='game/mods/guns')
    ap.add_argument('--fx', default='game/mods/cod4/fx')
    ap.add_argument('weapons', nargs='*')
    a = ap.parse_args()

    iwds = sorted(glob.glob(os.path.join(a.mw1, 'main', 'iw_*.iwd')))
    if not iwds:
        sys.exit(f'no iw_*.iwd under {a.mw1}/main')
    zips = [zipfile.ZipFile(p) for p in iwds]
    files = {}
    for zf in zips:                     # later archives override earlier ones
        for n in zf.namelist():
            files[n.lower()] = zf
    wfiles = {n.split('/')[-1]: n for n in files if n.startswith('weapons/mp/') and n.split('/')[-1]}

    def weapon(name):
        kv = files[wfiles[name]].read(wfiles[name]).decode('latin1').split('\\')
        return dict(zip(kv[1::2], kv[2::2]))

    guns = sorted(n for n in wfiles if weapon(n).get('weaponType') == 'bullet'
                  and weapon(n).get('weaponClass') in ('rifle', 'smg', 'mg', 'spread', 'pistol')
                  and weapon(n).get('displayName') not in ('WEAPON_CLAYMORE', 'WEAPON_DEFAULTWEAPON',
                                                          'WEAPON_C4_DETONATOR', 'WEAPON_LOCATION_SELECTOR')
                  and weapon(n).get('gunModel', '').startswith('viewmodel_'))
    if not a.weapons:
        print('Extractable guns:', ' '.join(guns))
        return
    print('loading common_mp.ff ...')
    z = load_zone(os.path.join(a.mw1, 'zone', 'english', 'common_mp.ff'))
    zone = Zone(z)

    def images(name):
        n = f'images/{name.lower()}.iwi'
        return iwi_to_dds(files[n].read(n)) if n in files else None

    # muzzle flash, tracer and bullet impact sprites for the shooter's effects
    os.makedirs(a.fx, exist_ok=True)
    for out, img in (('muzflash', 'muzflash2'), ('muzflash_side', 'ak47_flares'), ('tracer', 'tracer#0'),
                     ('impact_dust', 'bullethit_sand'), ('impact_spark', 'exp_spark')):
        dds = images(img)
        if dds:
            open(os.path.join(a.fx, out + '.dds'), 'wb').write(dds)

    print('loading localized_common_mp.ff (sounds) ...')
    lz = load_zone(os.path.join(a.mw1, 'zone', 'english', 'localized_common_mp.ff'))
    # bullet impact sounds, next to the impact sprites
    for out, alias in (('impact', 'bullet_small_concrete'), ('flesh', 'bullet_small_flesh')):
        write_wav(lz, alias, os.path.join(a.fx, out + '.wav'))

    for w in a.weapons:
        if w not in wfiles:
            print(f'{w}: no such weapon file'); continue
        d = weapon(w)
        # the third-person gun, textured like the first-person one
        model = zone.models.get(d.get('worldModel', '').lower())
        if not model:
            print(f'{w}: world model {d.get("worldModel")} not found in common_mp.ff'); continue
        folder = os.path.join(a.out, w)
        os.makedirs(folder, exist_ok=True)
        flash = model['base'].get('tag_flash', (0.0, 0.0, 0.0))
        with open(os.path.join(folder, 'weapon.ini'), 'w') as f:
            f.writelines(f'{k}={",".join(v.split())}\n' for k, v in d.items())
            f.write('x_flash=%.3f,%.3f,%.3f\n' % flash)     # our key: the muzzle, model space
        print(f'{w}: {model["name"]}')
        write_obj(z, zone, model, folder, images, set(d.get('hideTags', '').split()),
                  gun_image(d.get('gunModel', model['name']), files))
        notes = dict(zip(*[iter(d.get('notetrackSoundMap', '').split())] * 2))
        for out, aliases in (('fire', [d.get('fireSoundPlayer'), d.get('fireSound')]),
                             ('dry', [d.get('emptyFireSoundPlayer'), d.get('emptyFireSound')]),
                             ('raise', [d.get('raiseSoundPlayer'), d.get('raiseSound')]),
                             ('clipout', [v for k, v in notes.items() if 'clipout' in k]),
                             ('clipin', [v for k, v in notes.items() if 'clipin' in k]),
                             ('chamber', [v for k, v in notes.items() if 'chamber' in k or 'bolt' in k]),
                             # shell by shell (segmentedReload) and the bolt or pump between shots
                             ('start', [v for k, v in notes.items() if k.endswith('_start_plr')]),
                             ('loop', [v for k, v in notes.items() if k.endswith('_loop_plr')]),
                             ('end', [v for k, v in notes.items() if k.endswith('_end_plr')]),
                             ('rechamber', [d.get('rechamberSoundPlayer'), d.get('rechamberSound')]
                              + [v for k, v in notes.items() if 'rechamber' in k or 'pump' in k])):
            for alias in filter(None, aliases):
                if write_wav(lz, alias, os.path.join(folder, out + '.wav')):
                    print(f'  {out}.wav <- {alias}')
                    break
            else:
                print(f'  {out}: no loaded sound among {aliases}')


if __name__ == '__main__':
    main()
