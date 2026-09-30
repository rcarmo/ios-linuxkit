#!/usr/bin/env bun
/** Check selected technical-writing anti-tropes in maintained prose.
 * Excludes fenced code, quotations, tables, generated/vendor and dated reports.
 * A pass does not replace technical review or detect every rhetorical pattern.
 */
import {readFileSync,readdirSync} from 'node:fs';
import {join,relative,resolve} from 'node:path';
export const root=resolve(import.meta.dir,'..');
export const rules:[string,RegExp][]=[
 ['filler',/\b(it is worth noting|it bears mentioning|in order to|that said|for the avoidance of doubt|here is the (?:thing|kicker))\b/i],
 ['sincerity',/\b(honestly|to be honest|truth be told|the honest truth|in all honesty)\b/i],
 ['inflation',/\b(seamless(?:ly)?|holistic|transformative|enterprise-grade|best-in-class|state-of-the-art|synergy|tapestry|delve|revolutionise)\b/i],
 ['editorial claim',/\b(the (?:key point|main takeaway|important distinction)|this document (?:shows|proves)|we do not claim|do not claim|does not imply|should not be read as)\b/i],
 ['passive status',/\b(remains? (?:deferred|blocked|pending)|is currently planned|has been deferred)\b/i],
 ['stock framing',/\b(let us (?:break|unpack|dive)|think of it as|imagine a world|in conclusion|to sum up|in summary)\b/i],
 ['manufactured contrast',/\b(?:it|this) is not .{1,70}(?:;\s*it is|,?\s+but)\b/i],
 ['manufactured contrast',/\b(?:it|this) is .{1,60}, not\b/i],
 ['generic heading',/^#{1,6}\s+(?:Overview|Introduction|Conclusion|Summary)\s*$/i],
];
export function inspect(text:string){const hits:{line:number,rule:string,text:string}[]=[];let fence=false;
 for(const [i,line] of text.split('\n').entries()){
 if(/^\s*(```|~~~)/.test(line)){fence=!fence;continue;}
 if(fence||/^\s*(>|\|)/.test(line))continue;
 const prose=line.replace(/`[^`]*`/g,'').replace(/\]\([^)]*\)/g,']');
 for(const [name,pattern] of rules)if(pattern.test(prose))hits.push({line:i+1,rule:name,text:line.trim()});
 }return hits;
}
export function maintained(){return ['README.md','SECURITY.md',...readdirSync(join(root,'docs')).filter(n=>n.endsWith('.md')).map(n=>'docs/'+n)];}
if(import.meta.main){const files=process.argv.slice(2);const paths=files.length?files:maintained();let count=0;
 for(const p of paths){const file=resolve(root,p);for(const h of inspect(readFileSync(file,'utf8'))){console.error(`${relative(root,file)}:${h.line}: ${h.rule}: ${h.text}`);count++;}}
 if(count){console.error(`${count} style findings; review and rewrite prose.`);process.exit(1);}
 console.log(`Checked ${paths.length} maintained documents for selected anti-trope patterns.`);
}
