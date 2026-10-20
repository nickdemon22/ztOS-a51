'use strict';
// Private stdin/stdout RPC: no TCP listener and no message logging.
const path = require('path');
const readline = require('readline');
const fs = require('fs');
const {Client, LocalAuth} = require('whatsapp-web.js');
const QRCode = require('qrcode');
const root = process.env.ZTOS_WHATSAPP_DATA;
fs.mkdirSync(root, {recursive:true, mode:0o700});
process.umask(0o077);
function eco(pid) {
 try {
  const fd=fs.openSync('/run/zrnperfd.ctl',fs.constants.O_WRONLY|fs.constants.O_NONBLOCK);
  try {fs.writeSync(fd,`policy ${pid} eco\n`);} finally {fs.closeSync(fd);}
 } catch (_) {} // also runs on desktops without the ztOS power manager
}
eco(process.pid);
const emit = obj => process.stdout.write(JSON.stringify(obj)+'\n');
console.log = () => {}; // libraries must not corrupt the RPC stream
const client = new Client({
 authStrategy:new LocalAuth({dataPath:path.join(root,'session')}),
 puppeteer:{executablePath:'/usr/bin/chromium',headless:true,
  args:['--disable-dev-shm-usage','--disable-extensions','--disable-background-networking','--mute-audio']},
});
let ready=false;
const message = m => ({id:m.id._serialized,body:m.body || (m.hasMedia?'[Media]':'['+m.type+']'),fromMe:m.fromMe,timestamp:m.timestamp,author:m.author||m.from});
client.on('qr', async qr => {
 try {emit({event:'qr',image:await QRCode.toDataURL(qr,{width:280,margin:2})});}
 catch(e){emit({event:'error',error:e.message});}
});
client.on('authenticated',()=>emit({event:'status',text:'Linked. Loading chats…'}));
client.on('ready',()=>{
 const browser=client.pupBrowser && client.pupBrowser.process();
 if(browser) eco(browser.pid);
 ready=true;emit({event:'ready'});
});
client.on('auth_failure',()=>emit({event:'error',error:'Link failed. Reopen the app to try again.'}));
client.on('disconnected',reason=>{ready=false;emit({event:'error',error:'Disconnected: '+String(reason)});});
client.on('message_create',m=>emit({event:'message',chat:m.fromMe?m.to:m.from,message:message(m)}));
readline.createInterface({input:process.stdin}).on('line',async line=>{
 let req;
 try {
  req=JSON.parse(line);
  if(!ready) throw Error('WhatsApp is still connecting');
  let result;
  if(req.method==='chats') {
   const chats=await client.getChats();
   result=chats.filter(c=>!c.isReadOnly).sort((a,b)=>(b.timestamp||0)-(a.timestamp||0)).slice(0,100).map(c=>({id:c.id._serialized,name:c.name||c.id.user,unread:c.unreadCount,last:c.lastMessage?c.lastMessage.body:''}));
  } else if(req.method==='messages') {
   const chat=await client.getChatById(req.chat);
   result=(await chat.fetchMessages({limit:50})).map(message);
  } else if(req.method==='send') {
   if(typeof req.text!=='string'||!req.text.trim()||req.text.length>10000) throw Error('Enter a message of up to 10,000 characters');
   result=message(await client.sendMessage(req.chat,req.text,{sendSeen:false}));
  } else throw Error('Unknown operation');
  emit({id:req.id,result});
 } catch(e){emit({id:req&&req.id,error:e.message});}
});
let stopping=false;
async function stop(){if(stopping)return;stopping=true;await client.destroy().catch(()=>{});process.exit(0);}
process.on('SIGTERM',stop);process.on('SIGINT',stop);process.stdin.on('end',stop);
client.initialize().catch(e=>emit({event:'error',error:e.message}));
