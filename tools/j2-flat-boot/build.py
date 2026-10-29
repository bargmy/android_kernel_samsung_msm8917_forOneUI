"""Build the j2y18lte flat-system test boot from a newly compiled zImage.

The reference kernel is omitted from the template. The reference first-stage
init and QCDT are retained; the Android 10 second-stage init lives in system.
"""
import argparse, copy, gzip, hashlib, json, struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
PARTITION_SIZE = 33554432

def build(kernel_path, output_path):
    template = (HERE/'reference-boot-no-kernel.img.gz').read_bytes()
    provenance = json.loads((HERE/'provenance.json').read_text())
    assert hashlib.sha256(template).hexdigest() == provenance['template_sha256'], 'Template checksum mismatch'
    boot = gzip.decompress(template)
    assert boot[:8]==b'ANDROID!'
    fields=struct.unpack_from('<10I',boot,8)
    ksize,_,rsize,_,ssize,_,_,page,dtsize,_=fields
    assert ksize == 0 and page in (2048, 4096), 'Unexpected reference header'
    align=lambda n:(n+page-1)//page*page
    roff=page+align(ksize)
    soff=roff+align(rsize)
    dtoff=soff+align(ssize)
    kernel=kernel_path.read_bytes()
    assert len(kernel) >= 48 and kernel[36:40] == bytes.fromhex('18286f01'), 'Expected ARM zImage'
    header_kernel = bytearray(boot[:page])
    struct.pack_into('<I', header_kernel, 8, len(kernel))
    second=boot[soff:soff+ssize]
    dt=boot[dtoff:dtoff+dtsize]
    assert dt[:4]==b'QCDT'
    assert hashlib.sha256(dt).hexdigest() == provenance['reference_dt_sha256'], 'Reference QCDT changed'
    assert b'android,fstab' not in dt, 'Device tree fstab needs a separate review'
    ramdisk=gzip.decompress(boot[roff:roff+rsize])
    entries={}
    pos=0
    while pos+110<=len(ramdisk):
        h=ramdisk[pos:pos+110]
        assert h[:6] in (b'070701',b'070702')
        v=[int(h[i:i+8],16) for i in range(6,110,8)]
        name=ramdisk[pos+110:pos+110+v[11]-1].decode()
        start=(pos+110+v[11]+3)&~3
        data=ramdisk[start:start+v[6]]
        pos=(start+v[6]+3)&~3
        if name=='TRAILER!!!':break
        entries[name]={'values':v,'data':data}
    original_entries=copy.deepcopy(entries)

    def ramfile(name, data=b'', mode=0o100644):
        if isinstance(data,str):data=data.encode()
        v=[0,mode,0,0,2 if mode & 0xf000==0x4000 else 1,0,len(data),0,0,0,0,len(name.encode())+1,0]
        entries[name]={'values':v,'data':data}

    # Keeping the mount point name different from /system avoids Android 11
    # first-stage init's automatic SwitchRoot("/system"). /system remains a symlink.
    fstab=entries['fstab.qcom']['data'].decode()
    old='/dev/block/platform/soc/7824900.sdhci/by-name/system        /system'
    assert old in fstab
    fstab=fstab.replace(old,'/dev/block/platform/soc/7824900.sdhci/by-name/system        /j2_system',1)
    assert len([l for l in fstab.splitlines() if not l.startswith('#') and '/j2_system' in l])==1
    entries['fstab.qcom']['data']=fstab.encode()
    for name in list(entries):
        if name=='system' or name.startswith('system/'):
            del entries[name]
    ramfile('j2_system',mode=0o40755)
    ramfile('j2_system/etc',mode=0o40755)
    ramfile('j2_system/etc/ramdisk',mode=0o40755)
    if 'system/etc/ramdisk/build.prop' in original_entries:
        entries['j2_system/etc/ramdisk/build.prop']=original_entries['system/etc/ramdisk/build.prop']
    ramfile('system','/j2_system',0o120777)
    for name in ['acct','apex','cache','config','data','efs','mnt','odm','oem','postinstall','res','storage','sys','dev','proc','metadata']:
        if name not in entries:ramfile(name,mode=0o40755)
    for name,target in {'vendor':'/system/vendor','product':'/system/product','bin':'/system/bin','etc':'/system/etc','default.prop':'system/etc/prop.default','d':'/sys/kernel/debug','sdcard':'/storage/self/primary','persist':'/mnt/vendor/persist'}.items():
        ramfile(name,target,0o120777)
    ramfile('init.rc',(HERE/'ramdisk/init.rc').read_bytes(),0o100750)
    ramfile('init.environ.rc',(HERE/'ramdisk/init.environ.rc').read_bytes(),0o100750)
    ramfile('ueventd.rc',(HERE/'ramdisk/ueventd.rc').read_bytes(),0o100644)
    assert entries['init']['data']==original_entries['init']['data']

    cpio=bytearray()
    for index,(name,item) in enumerate(sorted(entries.items()),1):
        v=item['values'].copy()
        v[0]=index
        v[6]=len(item['data'])
        v[11]=len(name.encode())+1
        cpio+=b'070701'+''.join(f'{x:08x}' for x in v).encode()+name.encode()+b'\0'
        cpio+=b'\0'*((-len(cpio))%4)
        cpio+=item['data']
        cpio+=b'\0'*((-len(cpio))%4)
    name=b'TRAILER!!!\0'
    v=[len(entries)+1,0,0,0,1,0,0,0,0,0,0,len(name),0]
    cpio+=b'070701'+''.join(f'{x:08x}' for x in v).encode()+name
    cpio+=b'\0'*((-len(cpio))%512)
    packed=gzip.compress(bytes(cpio),mtime=0)
    header=header_kernel
    struct.pack_into('<I',header,16,len(packed))
    cmdline=(boot[64:576]+boot[608:1632]).split(b'\0',1)[0].decode()
    cmdline=' '.join(x for x in cmdline.split() if not x.startswith(('androidboot.selinux=','androidboot.init_rc=')))
    cmdline+=' androidboot.selinux=permissive androidboot.init_rc=/system/etc/j2-rootdir/init.rc'
    assert len(cmdline.encode())<512
    header[64:576]=cmdline.encode().ljust(512,b'\0')
    header[608:1632]=b'\0'*1024
    sha=hashlib.sha1()
    for data in (kernel,packed,second,dt):
        sha.update(data)
        sha.update(struct.pack('<I',len(data)))
    header[576:608]=sha.digest().ljust(32,b'\0')
    newboot=bytes(header)
    for data in (kernel,packed,second,dt):
        newboot+=data+b'\0'*(align(len(data))-len(data))
    assert len(newboot)<=PARTITION_SIZE, 'Boot image exceeds target partition'
    newboot=newboot.ljust(PARTITION_SIZE,b'\0')
    bootout=output_path
    bootout.write_bytes(newboot)

    print(json.dumps({'output':str(bootout), 'sha256':hashlib.sha256(newboot).hexdigest(),
                      'kernel_sha256':hashlib.sha256(kernel).hexdigest(),
                      'dt_sha256':hashlib.sha256(dt).hexdigest(), 'cmdline':cmdline}, indent=2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build(args.kernel, args.output)
