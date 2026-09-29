#!/usr/bin/env bun
/** Compare complete logical fakefs contents/guest metadata, independent of host inodes.
 * Operate on offline COPIES: SQLite may create/update SHM even with a read-only DB.
 */
import {Database} from 'bun:sqlite';import {statSync} from 'node:fs';import {join,resolve} from 'node:path';import {createHash} from 'node:crypto';import {sha} from '../../../tools/jit_aot/kit';
const [a,b,o]=process.argv.slice(2);if(!o)throw Error('fakefs-identity.ts SNAPSHOT_COPY IMPORT_COPY RESULT.json');
async function identity(root:string){
 const db=new Database(join(root,'meta.db'),{readonly:true});
 if((db.query('pragma integrity_check').get() as any).integrity_check!=='ok')throw Error('SQLite integrity');
 const rows=db.query('select path, inode, stat from paths join stats using(inode) order by path').all() as {path:Uint8Array,inode:number,stat:Uint8Array}[];
 const links=new Map<number,string[]>();for(const r of rows){const path=Buffer.from(r.path).toString();const l=links.get(r.inode)||[];l.push(path);links.set(r.inode,l);}
 const result=[];for(const r of rows){const path=Buffer.from(r.path).toString();const p=join(root,'data',path);const s=statSync(p);result.push({path,stat:Buffer.from(r.stat).toString('hex'),hardlinks:links.get(r.inode),data:s.isDirectory()?null:await sha(p),size:s.isDirectory()?null:s.size});}
 db.close();return result;
}
const aa=await identity(resolve(a)),bb=await identity(resolve(b));const digest=(x:unknown)=>createHash('sha256').update(JSON.stringify(x)).digest('hex');
const da=digest(aa),dd=digest(bb);const differences=aa.filter((r,i)=>JSON.stringify(r)!==JSON.stringify(bb[i])).slice(0,10);
await Bun.write(resolve(o),JSON.stringify({status:da===dd?'pass':'fail',snapshotPaths:aa.length,importPaths:bb.length,snapshotDigest:da,importDigest:dd,differences,scope:'all logical paths, guest stat blobs, hardlink groups, data bytes and sizes; ignores host inode IDs and host times'},null,2)+'\n');
if(da!==dd)throw Error('logical fakefs differs; see '+o);console.log('fakefs identity: '+aa.length+' paths, exact metadata/content/hardlink groups');
