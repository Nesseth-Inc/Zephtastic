#!/usr/bin/env python3
"""Immutable-source offline regression runner; target builds are separate."""
from concurrent.futures import ThreadPoolExecutor,as_completed
from pathlib import Path,PurePosixPath
import hashlib,json,os,re,subprocess,sys
import argparse
p=argparse.ArgumentParser(description="Run the frozen source offline regression suites. Never opens a hardware port.")
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument('--expected-version',required=True,help='exact firmware version expected by the stock client fixture')
p.add_argument('--expected-source-revision',required=True,help='exact full source commit ID or exported source_revision')
p.add_argument('--deps', required=True, type=Path)
p.add_argument('--tf-psa', type=Path, help='Override the pinned crypto dependency location')
p.add_argument('--ninja', type=Path, help='Optional Ninja executable for PSA host build')
p.add_argument('--output', required=True, type=Path)
p.add_argument('--client-python', default=sys.executable)
p.add_argument('--openssl', type=Path, default=Path('/opt/homebrew/opt/openssl@3'))
p.add_argument('--stock-root', required=True, type=Path,
               help='Exact pinned Meshtastic firmware checkout used by stock differential tests')
p.add_argument('--only', action='append', metavar='NAME',
               help='Run only this catalog job (repeatable); required prerequisites are added automatically')
a=p.parse_args()
repo=a.source.resolve();app=repo/'mt_only';deps=a.deps.resolve();out=a.output.resolve();stock=a.stock_root.resolve()
if not re.fullmatch(r'[0-9a-f]{40}',a.expected_source_revision) or not a.expected_version:
 raise RuntimeError('Expected source revision and firmware version must be explicit')
if out.is_relative_to(repo):raise RuntimeError('Output must be outside source tree')
def git(*args,cwd):
 return subprocess.check_output(['git','-C',str(cwd),*args],text=True).strip()
stock_revision=git('rev-parse','HEAD',cwd=stock)
if stock_revision!='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c':
 raise RuntimeError('Stock oracle must be exact commit 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c')
if git('status','--porcelain=v1','--untracked-files=all',cwd=stock):
 raise RuntimeError('Stock oracle worktree must be clean')
is_git=(repo/'.git').exists()
source_manifest=None;manifest_hash=None
if is_git:
 revision=git('rev-parse','HEAD',cwd=repo)
 if git('status','--porcelain=v1','--untracked-files=all',cwd=repo):
  raise RuntimeError('Source worktree must be clean before catalog execution')
 tracked=[os.fsdecode(x) for x in subprocess.check_output(
  ['git','-C',str(repo),'ls-files','-z']).split(b'\0') if x]
else:
 manifest_path=repo.parent/'SOURCE-MANIFEST.json'
 source_manifest=json.loads(manifest_path.read_text())
 manifest_hash=hashlib.sha256(manifest_path.read_bytes()).hexdigest()
 revision=source_manifest.get('source_revision')
 entries=source_manifest.get('files')
 if not isinstance(revision,str) or not isinstance(entries,list):
  raise RuntimeError('Source manifest has an invalid revision or files list')
 tracked=[];seen=set()
 for item in entries:
  if not isinstance(item,dict):raise RuntimeError('Source manifest entry must be an object')
  raw=item.get('path');digest=item.get('public_sha256')
  if not isinstance(raw,str) or not raw or '\\' in raw or '\0' in raw:
   raise RuntimeError('Unsafe source manifest path')
  rel=PurePosixPath(raw)
  if rel.is_absolute() or any(part in ('','..','.') for part in rel.parts) or rel.as_posix()!=raw:
   raise RuntimeError('Unsafe source manifest path: '+raw)
  normalized=rel.as_posix()
  if normalized in seen:raise RuntimeError('Duplicate source manifest path: '+normalized)
  seen.add(normalized)
  if not isinstance(digest,str) or not re.fullmatch(r'[0-9a-fA-F]{64}',digest):
   raise RuntimeError('Invalid source manifest digest: '+normalized)
  path=(repo/normalized).resolve()
  if not path.is_relative_to(repo.resolve()):raise RuntimeError('Source manifest path escapes source root: '+normalized)
  if not path.is_file():raise RuntimeError('Selected source manifest file is missing: '+normalized)
  if hashlib.sha256(path.read_bytes()).hexdigest().lower()!=digest.lower():
   raise RuntimeError('Source-manifest mismatch: '+normalized)
  tracked.append(normalized)
 actual={p.relative_to(repo).as_posix() for p in repo.rglob('*') if p.is_file()}
 if actual!=set(tracked):raise RuntimeError('Exported source inventory differs from the manifest')
if revision!=a.expected_source_revision:
 raise RuntimeError('Source revision does not match --expected-source-revision')
if out.exists():raise RuntimeError('Output directory must be fresh')
python=sys.executable
jobs={}
# Stock channel AES-CCM differential and same-size Config migration.
for suffix,flags in [('',[]),('-short',['--short-enums'])]:
 name='channel-aead'+suffix
 jobs[name]=[python,str(app/'scripts/test-channel-aead.py'),'--stock-root',str(stock),
             *flags,'--output',str(out/name/'checks')]
for label,folder in [('m186-role-intent','m186_role_intent'),('m186-hash-zero','m186_hash_zero')]:
 jobs[label]=[python,str(app/'tests'/folder/'run.py'),'--source',str(repo),
  '--expected-head',revision,'--output',str(out/label/'checks')]
jobs['m143-admin-authorization']=[python,str(app/'tests/run_m143_admin_authorization.py')]
jobs['m175-router-role-defaults']=[python,str(app/'tests/run_m175_router_role_defaults.py'),
 '--run','--source',str(repo),'--output',str(out/'m175-router-role-defaults/checks')]
# M102 desired Bluetooth config, verified app journal and transient PIN runtime.
for suffix,flags in [('',[]),('-short',['--short-enums'])]:
 for label,script,extra in [('bluetooth-settings','test-bluetooth-settings.py',[]),
                            ('bluetooth-runtime','test-ble-runtime.py',[]),
                            ('bluetooth-admin','test-companion.py',['--bluetooth-config']),
                            ('bluetooth-commit','test-companion.py',['--bluetooth-commit'])]:
  name=label+suffix
  jobs[name]=[python,str(app/'scripts'/script),*extra,*flags,'--output',str(out/name/'checks')]
# Retain source-owned M100 Neighbor and M101 storage/contact regressions in the catalog.
for suffix,flags in [('',[]),('-short',['--short-enums'])]:
 for name,script,args in [('neighbor-components','test-neighbor-query.py',[]),('neighbor-public','test-companion.py',['--neighbor-transactions'])]:
  label=name+suffix
  jobs[label]=[python,str(app/'scripts'/script),*args,*flags,'--output',str(out/label/'checks')]
jobs['pki-append-history']=[python,str(app/'scripts/test-pki-ledger.py'),'--cases',str(app/'tests/append_history_cases.cpp'),'--output',str(out/'pki-append-history')]
jobs['pki-peer-set']=[python,str(app/'scripts/test-pki-peer-helpers.py'),'--cases',str(app/'tests/peer_set_cases.cpp'),'--output',str(out/'pki-peer-set')]
# M99 signed software modules use the explicit NoPrivateCrypto host harness.
# Each interval invocation runs literal-stock normal and event modes internally;
# these two enum jobs therefore cover all four interval profiles once.
for suffix,flags in [('',[]),('-short',['--short-enums'])]:
 name='module-signing'+suffix
 jobs[name]=[python,str(app/'scripts/test-module-signing.py'),*flags,'--output',str(out/name/'checks')]
 name='module-intervals'+suffix
 jobs[name]=[python,str(app/'scripts/test-module-intervals.py'),*flags,'--output',str(out/name/'checks')]
# M98 coherent channel, privacy, presentation and real local Admin integration.
for suffix,flags in [('',[]),('-short',['--short-enums'])]:
 name='channel-privacy'+suffix
 jobs[name]=[python,str(app/'scripts/test-channel-privacy.py'),*flags,'--output',str(out/name/'checks')]
 name='channel-privacy-transport'+suffix
 jobs[name]=[python,str(app/'scripts/test-companion.py'),'--channel-privacy-transport',*flags,'--output',str(out/name/'checks')]
# Coupled M97 normal-use policy and immutable volatile lifetimes.
for short in (False,True):
 suffix='-short' if short else '';flags=['--short-enums'] if short else []
 for mode in ('normal-use-policy','normal-use-timing','endpoint-lifetimes'):
  name=mode+suffix
  jobs[name]=[python,str(app/'scripts/test-companion.py'),'--'+mode,*flags,'--output',str(out/name)]
 name='routing-lifetimes'+suffix
 jobs[name]=[python,str(repo/'zephcore/bench/run-routing-lifetime-tests.py'),*flags,'--output',str(out/name/'checks')]
for name,script,args in [('settings-edit','test-companion',['--settings-edit']),('endpoint-differential','test-endpoint-differential',[]),('m8-independent','test-m8-independent',['--source',str(repo)]),('m8-independent-companion','test-m8-independent',['--source',str(repo),'--test','m8_independent_companion.cpp']),('usb-bootloader','test-usb-bootloader',[]),('service','test-host',[]),('metadata-matrix','test-metadata-matrix',[]),('nodeinfo-differential','test-nodeinfo-differential',[]),('reboot','test-companion',['--reboot']),('nodeinfo','test-companion',['--nodeinfo']),('node-cache','test-node-cache',[]),('phone-diag','test-companion',['--phone','--ble-diagnostics']),('ble-build','test-ble-build',['--zephyr',str(deps/'zephyr')]),('phone','test-companion',['--phone']),('endpoint','test-companion',['--endpoint']),('companion','test-companion',[]),('ble','test-ble-transport',[]),('persistence','test-persistence',[]),('ui','test-ui',[]),('display','test-display',[]),('native','test-native',['--zephyr',str(deps/'zephyr')]),('power','test-t096-power',['--zephyr',str(deps/'zephyr')]),('conformance','test-conformance',[]),('journal-independent','test-conformance',['--journal'])]:
 jobs[name]=[python,str(app/'scripts'/f'{script}.py'),*args,'--output',str(out/name)]
for name,script in [('storage-diagnostics','test-storage-diagnostics.py'),('storage-call-traces','test-storage-call-traces.py')]:
 for suffix,args in [('',[]),('-short',['--short-enums'])]:
  jobs[name+suffix]=[python,str(app/'scripts'/script),*args,'--output',str(out/(name+suffix))]
jobs['native-activity']=[python,str(app/'scripts/test-native-activity.py'),'--zephyr',str(deps/'zephyr'),'--output',str(out/'native-activity')]
for component in ('window','math','native','integration'):
 name='channel-'+component
 jobs[name]=[python,str(app/'tests/channel_metrics'/component/'run.py'),'--output',str(out/name/'checks')]
jobs['channel-integration-short']=[python,str(app/'tests/channel_metrics/integration/run.py'),'--short-enums','--output',str(out/'channel-integration-short/checks')]
# M63 volatile last-heard metadata: cache lifecycle and actual companion wire exports.
jobs['peer-last-heard-cache']=[python,str(app/'tests/peer_last_heard/run.py'),'--output',str(out/'peer-last-heard-cache/checks')]
for short in (False,True):
 suffix='-short' if short else '';flags=['--short-enums'] if short else []
 name='peer-last-heard-export'+suffix
 jobs[name]=[python,str(app/'tests/peer_last_heard/export_run.py'),*flags,'--output',str(out/name/'checks')]
# M66 volatile cached DeviceMetrics: actual cache/journal and matched-reply exports.
jobs['peer-device-metrics-cache']=[python,str(app/'tests/peer_device_metrics/run.py'),'--output',str(out/'peer-device-metrics-cache/checks')]
for short in (False,True):
 suffix='-short' if short else '';flags=['--short-enums'] if short else []
 name='peer-device-metrics-export'+suffix
 jobs[name]=[python,str(app/'tests/peer_device_metrics/export_run.py'),*flags,'--output',str(out/name/'checks')]
# M64 packet-bound fractional SNR: native status, queue attribution and wire exports.
for short in (False,True):
 suffix='-short' if short else '';flags=['--short-enums'] if short else []
 name='packet-snr'+suffix
 jobs[name]=[python,str(app/'tests/packet_snr/run.py'),*flags,'--output',str(out/name/'checks')]
jobs['device-config']=[python,str(app/'scripts/test-companion.py'),'--device-config','--output',str(out/'device-config')]
jobs['stock-device-config']=[a.client_python,str(app/'scripts/test-stock-device-config.py'),'--output',str(out/'stock-device-config')]
jobs['fixed-position']=[python,str(app/'scripts/test-companion.py'),'--fixed-position','--output',str(out/'fixed-position')]
jobs['fixed-position-short']=[python,str(app/'scripts/test-companion.py'),'--fixed-position','--short-enums','--output',str(out/'fixed-position-short')]
jobs['stock-fixed-position']=[a.client_python,str(app/'scripts/test-stock-fixed-position.py'),'--output',str(out/'stock-fixed-position')]
jobs['stock-fixed-position-short']=[a.client_python,str(app/'scripts/test-stock-fixed-position.py'),'--short-enums','--output',str(out/'stock-fixed-position-short')]
jobs['input-event']=[python,str(app/'scripts/test-companion.py'),'--input-event','--output',str(out/'input-event')]
jobs['input-event-short']=[python,str(app/'scripts/test-companion.py'),'--input-event','--short-enums','--output',str(out/'input-event-short')]
jobs['stock-input-event']=[a.client_python,str(app/'scripts/test-stock-input-event.py'),'--stock-root',str(stock),'--output',str(out/'stock-input-event')]
jobs['stock-input-event-short']=[a.client_python,str(app/'scripts/test-stock-input-event.py'),'--stock-root',str(stock),'--short-enums','--output',str(out/'stock-input-event-short')]
jobs['input-display-bridge']=[python,str(app/'scripts/test-input-display-bridge.py'),'--output',str(out/'input-display-bridge')]
jobs['input-display-bridge-short']=[python,str(app/'scripts/test-input-display-bridge.py'),'--short-enums','--output',str(out/'input-display-bridge-short')]
jobs['carousel']=[python,str(app/'scripts/test-companion.py'),'--carousel','--output',str(out/'carousel')]
jobs['carousel-short']=[python,str(app/'scripts/test-companion.py'),'--carousel','--short-enums','--output',str(out/'carousel-short')]
jobs['stock-carousel']=[a.client_python,str(app/'scripts/test-stock-carousel.py'),'--stock-root',str(stock),'--output',str(out/'stock-carousel')]
jobs['stock-carousel-short']=[a.client_python,str(app/'scripts/test-stock-carousel.py'),'--stock-root',str(stock),'--short-enums','--output',str(out/'stock-carousel-short')]
jobs['node-mute']=[python,str(app/'scripts/test-node-mute.py'),'--output',str(out/'node-mute')]
jobs['node-mute-short']=[python,str(app/'scripts/test-node-mute.py'),'--short-enums','--output',str(out/'node-mute-short')]
jobs['stock-node-mute']=[a.client_python,str(app/'scripts/test-stock-node-mute.py'),'--stock-root',str(stock),'--output',str(out/'stock-node-mute')]
jobs['stock-node-mute-short']=[a.client_python,str(app/'scripts/test-stock-node-mute.py'),'--stock-root',str(stock),'--short-enums','--output',str(out/'stock-node-mute-short')]
for short in (False,True):
 suffix='-short' if short else ''
 flags=['--short-enums'] if short else []
 jobs['favorite-noop'+suffix]=[python,str(app/'scripts/test-favorite-noop.py'),*flags,'--output',str(out/('favorite-noop'+suffix))]
 jobs['stock-favorite-noop'+suffix]=[a.client_python,str(app/'scripts/test-stock-favorite-noop.py'),'--stock-root',str(stock),*flags,'--output',str(out/('stock-favorite-noop'+suffix))]
for short in (False,True):
 suffix='-short' if short else '';flags=['--short-enums'] if short else []
 jobs['position-query'+suffix]=[python,str(app/'scripts/test-position-query.py'),*flags,'--output',str(out/('position-query'+suffix))]
 jobs['stock-position-query'+suffix]=[a.client_python,str(app/'scripts/test-stock-position-query.py'),'--stock-root',str(stock),*flags,'--output',str(out/('stock-position-query'+suffix))]
jobs['channel-mute']=[python,str(app/'scripts/test-companion.py'),'--channel-mute','--output',str(out/'channel-mute')]
jobs['channel-mute-short']=[python,str(app/'scripts/test-companion.py'),'--channel-mute','--short-enums','--output',str(out/'channel-mute-short')]
jobs['stock-channel-mute']=[a.client_python,str(app/'scripts/test-stock-channel-mute.py'),'--stock-root',str(stock),'--output',str(out/'stock-channel-mute')]
jobs['stock-channel-mute-short']=[a.client_python,str(app/'scripts/test-stock-channel-mute.py'),'--stock-root',str(stock),'--short-enums','--output',str(out/'stock-channel-mute-short')]
jobs['unchanged-settings']=[python,str(app/'scripts/test-companion.py'),'--unchanged-settings','--output',str(out/'unchanged-settings')]
jobs['unchanged-settings-short']=[python,str(app/'scripts/test-companion.py'),'--unchanged-settings','--short-enums','--output',str(out/'unchanged-settings-short')]
jobs['traceroute']=[python,str(app/'scripts/test-companion.py'),'--traceroute','--output',str(out/'traceroute')]
jobs['traceroute-short']=[python,str(app/'scripts/test-companion.py'),'--traceroute','--short-enums','--output',str(out/'traceroute-short')]
jobs['traceroute-transit']=[python,str(app/'scripts/test-companion.py'),'--traceroute-transit','--output',str(out/'traceroute-transit')]
jobs['traceroute-transit-short']=[python,str(app/'scripts/test-companion.py'),'--traceroute-transit','--short-enums','--output',str(out/'traceroute-transit-short')]
jobs['stock-traceroute']=[a.client_python,str(app/'scripts/test-stock-traceroute.py'),'--stock-root',str(stock),'--output',str(out/'stock-traceroute')]
jobs['target-entropy']=[python,str(app/'scripts/test-target-entropy.py'),'--zephyr-root',str(deps/'zephyr'),'--output',str(out/'target-entropy')]
jobs['pki-storage-nvs']=[python,str(app/'tests/pki_storage_nvs/run.py'),'--zephyr',str(deps/'zephyr'),'--output',str(out/'pki-storage-nvs')]
jobs['xiao-expanded-contacts-nvs']=[*jobs['pki-storage-nvs'][:-2],'--xiao-expanded-contacts','--cases',str(app/'tests/pki_storage_nvs/expanded_contact_cases.cpp'),'--output',str(out/'xiao-expanded-contacts-nvs')]
for name in ('entropy','storage'):
 jobs['m11-'+name+'-independent']=[python,str(app/'tests'/('run-m11-'+name+'-adversarial.py')),'--source',str(repo),'--output',str(out/('m11-'+name+'-independent'))]
jobs['m11-device-independent']=[python,str(app/'scripts/test-m11-independent-device.py'),'--source',str(repo),'--output',str(out/'m11-device-independent')]
jobs['m11-device-independent-short']=[python,str(app/'scripts/test-m11-independent-device.py'),'--source',str(repo),'--short-enums','--output',str(out/'m11-device-independent-short')]
jobs['device-config-short']=[python,str(app/'scripts/test-companion.py'),'--device-config','--short-enums','--output',str(out/'device-config-short')]
jobs['stock-device-config-short']=[a.client_python,str(app/'scripts/test-stock-device-config.py'),'--short-enums','--output',str(out/'stock-device-config-short')]
jobs['m11-nvs-independent']=[python,str(app/'tests/pki_storage_nvs/run.py'),'--zephyr',str(deps/'zephyr'),'--cases',str(app/'tests/m11_nvs_adversarial.cpp'),'--output',str(out/'m11-nvs-independent')]
jobs['pki-heap']=[python,str(app/'tests/pki_heap/run.py'),'--source',str(repo),'--tf-psa',str((a.tf_psa or deps/'modules/crypto/tf-psa-crypto').resolve()),'--output',str(out/'pki-heap')]
jobs['psa-threading']=[python,str(app/'scripts/test-psa-threading.py'),'--tf-psa',str((a.tf_psa or deps/'modules/crypto/tf-psa-crypto').resolve()),'--output',str(out/'psa-threading')]
jobs['m11-threading-independent']=[python,str(app/'tests/run-m11-psa-threading-adversarial.py'),'--source',str(repo),'--output',str(out/'m11-threading-independent')]
jobs['nvs-capacity']=[python,str(app/'tests/nvs_capacity/run.py'),'--zephyr',str(deps/'zephyr'),'--output',str(out/'nvs-capacity')]
jobs['nvs']=[python,str(app/'tests/nvs_backend/run.py'),'--nvs',str(deps/'zephyr/subsys/settings/src/settings_nvs.c'),'--store',str(deps/'zephyr/subsys/settings/src/settings_store.c'),'--app',str(app),'--kvss',str(deps/'zephyr/subsys/kvss/nvs/nvs.c'),'--output',str(out/'nvs')]
for name,script in [('routing','run-routing-core-tests'),('codec','run-mt-codec-tests'),('ctr-differential','run-routing-differential'),('signature-differential','run-mt-signature-tests')]:
 jobs[name]=[python,str(repo/'zephcore/bench'/f'{script}.py'),'--output',str(out/name)]
 if name=='codec':jobs[name]+=['--stock-root',str(stock)]
jobs['pki-offline']=[python,str(app/'experimental/pki/test.py'),'--openssl',str(a.openssl.resolve()),'--output',str(out/'pki-offline')]
jobs['m9-independent']=[python,str(app/'scripts/test-m9-independent.py'),'--source',str(repo),'--output',str(out/'m9-independent')]
jobs['m9-identity-session']=[python,str(app/'scripts/test-m9-identity-session.py'),'--pki-source',str(repo),'--identity-source',str(repo),'--openssl',str(a.openssl.resolve()),'--output',str(out/'m9-identity-session')]
jobs['pki-identity']=[python,str(app/'scripts/test-pki-identity.py'),'--output',str(out/'pki-identity')]
for name in ('ledger','signer'):
 jobs['pki-'+name]=[python,str(app/'scripts'/('test-pki-'+name+'.py')),'--output',str(out/('pki-'+name))]
 jobs['m10-'+name+'-independent']=[python,str(app/'tests'/('run-m10-'+name+'-adversarial.py')),'--source',str(repo),'--output',str(out/('m10-'+name+'-independent'))]
jobs['stock-private-client']=[a.client_python,str(app/'scripts/test-stock-private-client.py'),'--source',str(repo),'--output',str(out/'stock-private-client')]
jobs['client-export-provenance']=[python,str(app/'tests/stock-private-client/test_provenance.py')]
jobs['m10-endpoint-independent']=[python,str(app/'scripts/test-m8-independent.py'),'--source',str(repo),'--test','m10_endpoint_adversarial.cpp','--output',str(out/'m10-endpoint-independent')]
jobs['pki-psa']=[python,str(app/'tests/pki/run.py'),'--tf-psa',str((a.tf_psa or deps/'modules/crypto/tf-psa-crypto').resolve()),'--output',str(out/'pki-psa')]
if a.ninja:jobs['pki-psa']+=['--ninja',str(a.ninja.resolve())]
# M28-M46 companion behavior must remain in the standard regression catalog.
for short in (False,True):
 suffix='-short' if short else '';flags=['--short-enums'] if short else []
 for name,selector in [('telemetry-query',[]),('uptime-responder',['--responder']),('local-telemetry',['--local']),('local-reads',['--local-reads'])]:
  label=name+suffix
  jobs[label]=[python,str(app/'scripts/test-telemetry-query.py'),*selector,*flags,'--output',str(out/label)]
 label='stock-telemetry'+suffix
 jobs[label]=[a.client_python,str(app/'scripts/test-stock-telemetry-query.py'),*flags,'--stock-root',str(stock),'--output',str(out/label)]
 label='position-responder'+suffix
 jobs[label]=[python,str(app/'scripts/test-position-responder.py'),'--responder',*flags,'--output',str(out/label)]
 label='stock-local-reads'+suffix
 # The runner owns this parent for runner.log; the client requires a fresh directory.
 jobs[label]=[a.client_python,str(app/'scripts/test-stock-local-reads.py'),'--executable',str(out/('local-reads'+suffix)/'telemetry-query-tests'),'--output',str(out/label/'client')]
 for name in ('display-names','normal-tx'):
  label=name+suffix
  jobs[label]=[python,str(app/'scripts/test-companion.py'),'--'+name,*flags,'--output',str(out/label)]
jobs['airtime-process-cleanup']=[python,str(app/'tests/airtime_quality/test_bounded_process.py')]
# M60 measured TX airtime: real Service lifecycle and generated stock wire replies.
for short in (False,True):
 suffix='-short' if short else '';flags=['--short-enums'] if short else []
 for fixture in ('accounting','meter-quality','window-math','actual-responder','local-telemetry'):
  label='airtime-'+fixture+suffix
  jobs[label]=[a.client_python,str(app/'scripts/test-airtime-quality.py'),'--fixture',fixture,*flags,'--output',str(out/label)]
jobs['runtime-resources']=[python,str(app/'scripts/test-runtime-resources.py'),'--output',str(out/'runtime-resources'/'profiles')]
jobs['pki-usb-ingress']=[python,str(app/'tests/run-m12-provisioning.py'),'--cases',str(app/'tests/pki_usb_ingress_cases.cpp'),'--output',str(out/'pki-usb-ingress')]
jobs['pki-provisioning-nvs']=[python,str(app/'tests/pki_storage_nvs/run.py'),'--zephyr',str(deps/'zephyr'),'--cases',str(app/'tests/pki_storage_nvs/provision_cases.cpp'),'--output',str(out/'pki-provisioning-nvs')]
# M86 integrated client-data batch: exercise all new paths in the standard catalog.
for short in (False,True):
 suffix='-short' if short else '';flags=['--short-enums'] if short else []
 for name,selector in [('local-stats',['--local-stats']),('public-local-stats',['--public-local-stats']),('remote-telemetry',['--remote-variants'])]:
  label=name+suffix
  jobs[label]=[python,str(app/'scripts/test-telemetry-query.py'),*selector,*flags,'--output',str(out/label)]
 label='public-application'+suffix
 jobs[label]=[python,str(app/'scripts/test-companion.py'),'--public-application',*flags,'--output',str(out/label)]
 label='peer-position'+suffix
 jobs[label]=[python,str(app/'tests/peer_position/run.py'),*flags,'--output',str(out/label/'checks')]
env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1','ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'}
def run(name,command):
 (out/name).mkdir(exist_ok=True)
 with (out/name/'runner.log').open('w') as log:r=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT)
 print(name,'PASS' if r.returncode==0 else 'FAIL',flush=True)
 if r.returncode:print((out/name/'runner.log').read_text()[-3500:],flush=True)
 return {'name':name,'exit':r.returncode,'hardware':False}
jobs['module-settings']=[python,str(app/'scripts/test-module-settings.py'),'--output',str(out/'module-settings/checks')]
# Offline network/MQTT owners: these are native fixtures only; no live broker,
# Wi-Fi join, TLS socket, or hardware access is part of the catalog.
jobs['mqtt-gateway-settings']=[python,str(app/'tests/run-mqtt-gateway-settings.py')]
jobs['mqtt-gateway-service']=[python,str(app/'tests/run-mqtt-gateway-service.py')]
jobs['mqtt-gateway-contract']=[python,str(app/'tests/run-mqtt-gateway-contract.py')]
jobs['mqtt-client-proxy-service']=[python,str(app/'tests/run-mqtt-client-proxy-service.py')]
jobs['mqtt-private-downlink-service']=[python,str(app/'tests/run-mqtt-private-downlink-service.py')]
jobs['mqtt-private-downlink-admission']=[python,str(app/'tests/run-mqtt-private-downlink-admission.py')]
jobs['mqtt-private-uplink-service']=[python,str(app/'tests/run-mqtt-private-uplink-service.py'),'--source',str(repo)]
jobs['mqtt-pki-uplink']=[python,str(app/'tests/run-mqtt-pki-uplink.py')]
jobs['mqtt-publisher-transport']=[python,str(app/'tests/run-mqtt-publisher-transport.py')]
jobs['mqtt-service-envelope']=[python,str(app/'tests/run-mqtt-service-envelope-codec.py')]
jobs['mqtt-packet-eligibility']=[python,str(app/'tests/run-mqtt-packet-eligibility.py')]
jobs['mqtt-region-root-policy']=[python,str(app/'tests/run-mqtt-region-root-policy.py')]
jobs['mqtt-reconnect']=[python,str(app/'tests/run-mqtt-reconnect.py')]
jobs['mqtt-tls-clock']=[python,str(app/'tests/run-mqtt-tls-clock.py')]
jobs['mqtt-map-report']=[python,str(app/'tests/run-mqtt-map-report.py')]
jobs['network-offline']=[python,str(app/'scripts/test-network.py'),'--output',str(out/'network-offline/checks')]
jobs['wifi-driver-offline']=[python,str(app/'scripts/test-wifi-driver.py'),'--output',str(out/'wifi-driver-offline/checks')]
for short in (False,True):
 suffix='-short'if short else '';flags=['--short-enums']if short else []
 jobs['module-config'+suffix]=[python,str(app/'scripts/test-companion.py'),'--module-config',*flags,'--output',str(out/('module-config'+suffix))]
for suffix,flags in [('',[]),('-short',['--short-enums'])]:
 name='signed-public-application'+suffix
 jobs[name]=[python,str(app/'scripts/test-companion.py'),'--signed-public-application',*flags,'--output',str(out/name)]
# M91 persisted US modulation/channel selection and actual native adapter bodies.
for suffix,flags in [('',[]),('-short',['--short-enums'])]:
 name='interval-extended'+suffix
 jobs[name]=[python,str(app/'scripts/test-companion.py'),'--interval-extended',*flags,'--output',str(out/name)]
 name='radio-config'+suffix
 jobs[name]=[python,str(app/'scripts/test-companion.py'),'--radio-settings',*flags,'--output',str(out/name)]
 name='radio-settings-differential'+suffix
 jobs[name]=[python,str(app/'scripts/test-radio-settings.py'),'--stock-header',str(stock/'src/mesh/MeshRadio.h'),'--lora-header',str(deps/'zephyr/include/zephyr/drivers/lora.h'),*flags,'--output',str(out/name/'checks')]
# M93 combined role/name/persistence/normal-forwarding integration.
for suffix,flags in [('',[]),('-short',['--short-enums'])]:
 for label,extra in [('role-channel-config',['--config']),('role-channel-integration',[])]:
  name=label+suffix
  jobs[name]=[python,str(app/'scripts/test-role-channel.py'),*extra,*flags,'--output',str(out/name/'checks')]
# M95 actual channel bank, persistence/Companion/Service integration and collisions.
for suffix,flags in [("",[]),("-short",["--short-enums"])]:
 name='channel-bank'+suffix
 jobs[name]=[python,str(app/'scripts/test-channel-bank.py'),*flags,'--output',str(out/name/'checks')]
 name='manual-radio'+suffix
 jobs[name]=[python,str(app/'scripts/test-manual-radio.py'),*flags,'--output',str(out/name/'checks')]
# M94 channel-key expansion: keep exclusive child outputs below catalog logs.
for suffix,flags in [('',[]),('-short',['--short-enums'])]:
 name='channel-key'+suffix
 jobs[name]=[python,str(app/'scripts/test-channel-key.py'),*flags,'--output',str(out/name/'checks')]
# M94 configured hop range and literal stock response/default differential.
for suffix,flags in [('',[]),('-short',['--short-enums'])]:
 name='hop-policy'+suffix
 jobs[name]=[python,str(app/'scripts/test-hop-policy.py'),*flags,'--output',str(out/name/'checks')]
 name='hop-origin'+suffix
 jobs[name]=[python,str(app/'scripts/test-hop-policy.py'),'--origin',*flags,'--output',str(out/name/'checks')]

# PSA and protocol follow-ups are declared before inventory validation so their
# source files receive the same strict presence/hash treatment as initial jobs.
tf_psa=(a.tf_psa or deps/'modules/crypto/tf-psa-crypto').resolve()
psa_library=out/'pki-psa/psa-library/core/libtfpsacrypto.a'
followups={
 'm10-protocol-boundaries':[python,str(app/'tests/run-m10-protocol-boundaries.py'),'--source',str(repo),'--stock-build',str(out/'pki-offline'),'--openssl',str(a.openssl.resolve()),'--output',str(out/'m10-protocol-boundaries')],
 'm11-startup-independent':[python,str(app/'tests/run-m11-startup-adversarial.py'),'--source',str(repo),'--psa-root',str(tf_psa),'--psa-library',str(psa_library),'--output',str(out/'m11-startup-independent')],
 'pki-startup':[python,str(app/'scripts/test-pki-runtime.py'),'--startup','--psa-root',str(tf_psa),'--psa-library',str(psa_library),'--output',str(out/'pki-startup')],
 'pki-runtime':[python,str(app/'scripts/test-pki-runtime.py'),'--psa-root',str(tf_psa),'--psa-library',str(psa_library),'--output',str(out/'pki-runtime')],
 'm10-runtime-independent':[python,str(app/'tests/run-m10-runtime-adversarial.py'),'--source',str(repo),'--psa-root',str(tf_psa),'--psa-library',str(psa_library),'--output',str(out/'m10-runtime-independent')],
 'stock-private-runtime':[a.client_python,str(app/'scripts/test-stock-private-runtime.py'),'--source',str(repo),'--psa-root',str(tf_psa),'--psa-library',str(psa_library),'--output',str(out/'stock-private-runtime')],
 'm143-admin-snapshot':[python,str(app/'tests/run_m143_admin_snapshot.py'),'--source',str(repo),'--output',str(out/'m143-admin-snapshot/checks'),'--psa-root',str(tf_psa),'--psa-library',str(psa_library)],
 'm146-admin-mutation':[python,str(app/'tests/run_m146_admin_mutation_companion.py'),'--source',str(repo),'--output',str(out/'m146-admin-mutation/checks'),'--psa-root',str(tf_psa),'--psa-library',str(psa_library)],
}
for label,case in [('pki-private-metrics','pki_metrics_cases.cpp'),('pki-primary-channel-runtime','primary_channel_runtime_cases.cpp'),('pki-public-application','public_application_runtime_cases.cpp'),('pki-private-application','private_application_runtime_cases.cpp'),('pki-module-runtime','module_runtime_cases.cpp'),('xiao-expanded-contacts-runtime','expanded_contact_runtime.cpp'),('pki-contact-cache','contact_cache_cases.cpp'),('pki-multi-peer','multi_peer_runtime_cases.cpp'),('pki-long-storage','long_storage_integration_cases.cpp'),('pki-neighbor','pki_neighbor_cases.cpp')]:
 followups[label]=[python,str(app/'scripts/test-pki-runtime.py'),'--cases',str(app/'tests'/case),'--psa-root',str(tf_psa),'--psa-library',str(psa_library),'--output',str(out/label)]
for suffix,extra in [('',[]),('-short',['--short-enums'])]:
 name='m186-psa-classification'+suffix
 followups[name]=[python,str(app/'scripts/test-pki-runtime.py'),'--cases',str(app/'tests/m186_psa_classification_cases.cpp'),
  *extra,'--psa-root',str(tf_psa),'--psa-library',str(psa_library),'--output',str(out/name)]
followups['pki-private-metrics-short']=[*followups['pki-private-metrics'][:-2],'--short-enums','--output',str(out/'pki-private-metrics-short')]
followups['pki-private-application-short']=[*followups['pki-private-application'][:-2],'--short-enums','--output',str(out/'pki-private-application-short')]
followups['pki-contact-cache-short']=[*followups['pki-contact-cache'][:-2],'--short-enums','--output',str(out/'pki-contact-cache-short')]
for label in ('pki-multi-peer','pki-neighbor','xiao-expanded-contacts-runtime'):
 followups[label+'-short']=[*followups[label][:-2],'--short-enums','--output',str(out/(label+'-short'))]
followup_prerequisites={name:['pki-psa'] for name in followups}
followup_prerequisites['m10-protocol-boundaries']=['pki-offline']

# Focused reruns are additive to the default catalog. Resolve dependencies
# before source inventory validation so every selected prerequisite remains
# subject to the same immutable snapshot checks as an ordinary full run.
catalog_names=set(jobs)|set(followups)|{'stock-python'}
requested_names=list(dict.fromkeys(a.only or []))
if requested_names:
 unknown=sorted(set(requested_names)-catalog_names)
 if unknown:raise RuntimeError('Unknown --only catalog job(s): '+', '.join(unknown))
 selected=set(requested_names)
 changed=True
 while changed:
  changed=False
  for name in tuple(selected):
   prerequisites=(['companion'] if name=='stock-python' else
                  followup_prerequisites.get(name,[]))
   for prerequisite in prerequisites:
    if prerequisite not in selected:
     selected.add(prerequisite);changed=True
 partial_catalog=True
 jobs={name:command for name,command in jobs.items() if name in selected}
 followups={name:command for name,command in followups.items() if name in selected}
 run_stock_python='stock-python' in selected
else:
 selected=set(jobs)|set(followups)|{'stock-python'}
 partial_catalog=False
 run_stock_python=True

# Bind snapshot coverage to every source file this selected catalog actually consumes.
selected_source=set();missing_selected=[]
inventory_commands=[*jobs.values(),*followups.values()]
if run_stock_python:
 inventory_commands.append([a.client_python,str(app/'scripts/test-python-client.py')])
for command in inventory_commands:
 for token in command:
  if not isinstance(token,str) or not Path(token).is_absolute():continue
  candidate=Path(token).resolve()
  if not candidate.is_relative_to(repo) or candidate==repo:continue
  if candidate.is_dir():continue
  relative=candidate.relative_to(repo).as_posix()
  if not candidate.is_file():missing_selected.append(relative)
  else:selected_source.add(relative)
if missing_selected:raise RuntimeError('Selected source file is missing: '+', '.join(sorted(set(missing_selected))))
tracked_set=set(tracked)
not_tracked=sorted(selected_source-tracked_set)
if not_tracked:raise RuntimeError('Selected source file is not in the tracked source snapshot: '+', '.join(not_tracked))
if not is_git:
 missing_manifest=sorted(selected_source-tracked_set)
 if missing_manifest:raise RuntimeError('Selected source file is absent from source manifest: '+', '.join(missing_manifest))
present=[n for n in tracked if (repo/n).is_file()]
missing=sorted(set(tracked)-set(present))
if missing:
 raise RuntimeError('Sparse checkout requires an immutable exported source manifest; missing tracked paths: '+', '.join(missing[:10]))
if not selected_source.issubset(set(present)):
 raise RuntimeError('Selected source file is absent from present source scope')
snapshot={n:hashlib.sha256((repo/n).read_bytes()).hexdigest() for n in present}
def verify():
 for n,digest in snapshot.items():
  if hashlib.sha256((repo/n).read_bytes()).hexdigest()!=digest:raise RuntimeError('Source changed during catalog: '+n)
if git('rev-parse','HEAD',cwd=stock)!=stock_revision or git('status','--porcelain=v1','--untracked-files=all',cwd=stock):
 raise RuntimeError('Stock oracle changed or became dirty during preflight')
if is_git and (git('rev-parse','HEAD',cwd=repo)!=revision or git('status','--porcelain=v1','--untracked-files=all',cwd=repo)):
 raise RuntimeError('Source checkout changed or became dirty during preflight')
verify()
out.mkdir(parents=True,exist_ok=False)
source_scope={'tracked_count':len(tracked),'present_count':len(present),
 'selected_source_count':len(selected_source),'selected_source_files':sorted(selected_source),
 'skipped_absent_paths':[]}
results=[]
# Some catalog members invoke a four-way CMake build internally. Run catalog
# members serially so the documented aggregate compiler ceiling remains four.
with ThreadPoolExecutor(max_workers=1) as pool:
 for f in as_completed([pool.submit(run,n,c) for n,c in jobs.items()]):results.append(f.result())
by_name={r['name']:r for r in results}
if run_stock_python and by_name.get('companion',{}).get('exit')==0:
 results.append(run('stock-python',[a.client_python,str(app/'scripts/test-python-client.py'),'--sim',str(out/'companion/companion-tests'),'--expected-version',a.expected_version,'--output',str(out/'stock-python/results.json')]))
elif run_stock_python:
 results.append({'name':'stock-python','exit':None,'status':'SKIPPED_PREREQUISITE',
  'prerequisites':['companion'],'reason':'companion fixture did not pass'})
for name,command in followups.items():
 prerequisites=followup_prerequisites[name]
 failed=[key for key in prerequisites if by_name.get(key,{}).get('exit')!=0]
 if failed:
  results.append({'name':name,'exit':None,'status':'SKIPPED_PREREQUISITE',
   'prerequisites':prerequisites,'failed_prerequisites':failed})
 else:
  results.append(run(name,command))
verify()
if is_git:
 if git('rev-parse','HEAD',cwd=repo)!=revision or git('status','--porcelain=v1','--untracked-files=all',cwd=repo):
  raise RuntimeError('Source revision or clean state changed during catalog execution')
if git('rev-parse','HEAD',cwd=stock)!=stock_revision or git('status','--porcelain=v1','--untracked-files=all',cwd=stock):
 raise RuntimeError('Stock oracle changed or became dirty during catalog execution')
if not is_git and hashlib.sha256((repo.parent/'SOURCE-MANIFEST.json').read_bytes()).hexdigest()!=manifest_hash:
 raise RuntimeError('Source manifest changed during catalog execution')
if not is_git and {p.relative_to(repo).as_posix() for p in repo.rglob('*') if p.is_file()}!=set(tracked):
 raise RuntimeError('Exported source inventory changed during catalog execution')
report={'expected_source_revision':a.expected_source_revision,'source_revision':revision,
 'expected_firmware_version':a.expected_version,'source_manifest_sha256':manifest_hash,
 'stock_revision':stock_revision,'stock_clean':True,'source_scope':source_scope,
 'requested_jobs':requested_names,'selected_jobs':sorted(selected),
 'auto_prerequisites':sorted(selected-set(requested_names)) if partial_catalog else [],
 'complete_catalog':not partial_catalog,
 'source_hashes_unchanged':True,'status':'PASS' if all(r['exit']==0 for r in results) else 'FAIL',
 'groups':results,'hardware':False}
(out/'SUMMARY.json').write_text(json.dumps(report,indent=2)+'\n');raise SystemExit(report['status']!='PASS')
