/* One loader save per ROM. Action timestamps belong to Pebbleboy, not files. */
module.exports = function(options) {
  var KEY = 'pbLoaderSaves.v1';
  var STATUS = 'pbSaveStatus.v1';
  var MAX_BYTES = 32768;
  var MAX_BASE64 = Math.ceil(MAX_BYTES / 3) * 4;
  var session = null, timer = null;
  var serial = (Date.now() & 0x3fffffff) || 1;
  var CMD = {REQUEST:50, INFO:51, READ:52, DATA:53, BEGIN:54, WRITE:55,
    COMMIT:56, ACK:57, ABORT:58, ERROR:59};
  function library() {
    var value = JSON.parse(localStorage.getItem(KEY) || 'null');
    if (!value) return {games:{}, bindings:{}, pending:{}};
    if (!value.games || !value.bindings || !value.pending) throw Error('Invalid save storage');
    return value;
  }
  function write(value) {
    var text = JSON.stringify(value);
    localStorage.setItem(KEY, text);
    if (localStorage.getItem(KEY) !== text) throw Error('Phone storage write failed');
  }
  function status(message) {
    console.log('pebbleboy saves: ' + message);
    try { localStorage.setItem(STATUS, message); } catch (_) {}
  }
  function validId(id) { return /^[0-9a-f]{40}$/.test(id || ''); }
  function validSize(size) { return [0,512,2048,8192,32768].indexOf(size) >= 0; }
  function decode(data, size) {
    if (typeof data !== 'string' || data.length>MAX_BASE64 ||
        !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(data)) {
      throw Error('Choose a .sav up to 32 KiB');
    }
    var bytes = options.fromBase64(data);
    if (!bytes.length || !validSize(bytes.length) || (size!=null && bytes.length!==size)) {
      throw Error(size!=null ? 'Expected a '+size+'-byte .sav' : 'Unsupported save size');
    }
    return bytes;
  }
  function savedBytes(save) {
    var bytes = decode(save.data,save.ramSize);
    if (options.crc32(bytes)!==save.crc32 || !validTime(save.updatedAt)) throw Error('Invalid stored save');
    return bytes;
  }
  function validTime(value) {
    return typeof value==='number' && isFinite(value) && value>=0 &&
      value<=9007199254740991 && value===Math.floor(value);
  }
  function timeBytes(value) {
    var out=[];
    for (var i=0;i<8;i++) {out.push(value%256); value=Math.floor(value/256);}
    return out;
  }
  function readTime(bytes) {
    if (!bytes || bytes.length!==8) throw Error('Update Pebbleboy to sync saves');
    var value=0;
    for (var i=7;i>=0;i--) value=value*256+bytes[i];
    if (!validTime(value)) throw Error('Invalid save timestamp');
    return value;
  }
  function romEntry(id) {
    var list=options.roms();
    for (var i=0;i<list.length;i++) if (list[i].id===id) return list[i];
    throw Error('ROM entry not found');
  }
  function bound(all,entry) {
    var binding=all.bindings[entry.id];
    return binding && binding.url===entry.url ? binding : null;
  }
  function entrySave(all,entry) {
    var pending=all.pending[entry.id];
    if (pending && pending.url===entry.url) return pending.save;
    var binding=bound(all,entry);
    return binding && all.games[binding.romId] ? all.games[binding.romId].save : null;
  }
  function replace(all,id,save) {
    var old=all.games[id] || {};
    // Keep a recovery copy internally; it is not another user-visible slot.
    if (old.save && (old.save.updatedAt!==save.updatedAt || old.save.data!==save.data)) old.recovery=old.save;
    old.save=save;
    all.games[id]=old;
  }
  function bind(entry,meta,ramSize) {
    if (!validId(meta.sha1) || !validSize(ramSize)) throw Error('Unsupported cartridge save size');
    var all=library();
    all.bindings[entry.id]={url:entry.url,romId:meta.sha1,ramSize:ramSize};
    if (!all.games[meta.sha1]) {
      // Read earlier preview saves without deleting their original storage.
      var legacy=JSON.parse(localStorage.getItem('pbSaveLibrary.v1') || '{}')[meta.sha1];
      if (legacy && legacy.current && legacy.current.ramSize===ramSize && ramSize) {
        var bytes=options.fromBase64(legacy.current.data);
        if (bytes.length===ramSize+24 && options.crc32(bytes)===legacy.current.crc32) {
          var ram=bytes.subarray(0,ramSize);
          replace(all,meta.sha1,{data:options.toBase64(ram),ramSize:ramSize,
            crc32:options.crc32(ram),updatedAt:0,origin:'watch'});
        }
      }
    }
    var pending=all.pending[entry.id];
    if (pending && pending.url===entry.url) {
      savedBytes(pending.save);
      if (pending.save.ramSize!==ramSize) {
        write(all); // Remember the discovered size so settings can explain the mismatch.
        throw Error('This ROM needs a '+ramSize+'-byte save');
      }
      var existing=all.games[meta.sha1] && all.games[meta.sha1].save;
      if (!existing || pending.save.updatedAt>=existing.updatedAt) replace(all,meta.sha1,pending.save);
      delete all.pending[entry.id];
    }
    write(all);
  }
  function stop() {
    if (timer) clearTimeout(timer);
    timer=null;
    session=null;
  }
  function releaseWatch(token) {
    Pebble.sendAppMessage({PB_CMD:CMD.ABORT,PB_BANK:token},function(){},function(){});
  }
  function finish(message,error) {
    var done=session && session.done;
    if (session) releaseWatch(session.token);
    stop();
    if (message) status(message);
    if (done) done(error || null);
  }
  function transmit() {
    if (!session) return;
    var current=session;
    Pebble.sendAppMessage(current.message,function(){},function(){});
    timer=setTimeout(function() {
      if (session!==current) return;
      if (++current.retries>3) finish('Waiting to sync with the watch.','Watch unavailable');
      else transmit();
    },1500);
  }
  function request(message) {
    if (timer) clearTimeout(timer);
    message.PB_BANK=session.token;
    session.message=message;
    session.retries=0;
    transmit();
  }
  function sync(done,pause) {
    if (session) {if (done) done('Sync already running'); return;}
    session={mode:'read',token:++serial,offset:0,done:done};
    request({PB_CMD:CMD.REQUEST,PB_OFFSET:pause ? 1 : 0});
  }
  function writeNext() {
    var offset=session.offset;
    if (offset===session.bytes.length) request({PB_CMD:CMD.COMMIT,PB_CRC32:session.crc});
    else {
      var end=offset===session.ramSize ? session.bytes.length : Math.min(offset+256,session.ramSize);
      request({PB_CMD:CMD.WRITE,PB_OFFSET:offset,PB_DATA:Array.prototype.slice.call(session.bytes.subarray(offset,end))});
    }
  }
  function reconcile() {
    var watch=session;
    var all=library();
    var phone=all.games[watch.id] && all.games[watch.id].save;
    var ram=watch.bytes.subarray(0,watch.ramSize);
    var watchSave=watch.filled && watch.ramSize ? {data:options.toBase64(ram),
      ramSize:watch.ramSize,crc32:options.crc32(ram),updatedAt:watch.updatedAt,origin:'watch'} : null;
    if (phone) savedBytes(phone);
    if (phone && phone.ramSize!==watch.ramSize) throw Error('Save size does not match the loaded ROM');
    var phoneWins=phone && (!watchSave || phone.updatedAt>watch.updatedAt ||
      (phone.updatedAt===watch.updatedAt && phone.origin==='import' && phone.data!==watchSave.data));
    if (phoneWins) {
      // Snapshot the overwritten watch copy before touching its active bank.
      if (watchSave) {
        all.games[watch.id].recovery=watchSave;
        write(all);
      }
      watch.bytes.set(savedBytes(phone),0); // Preserve the watch's current RTC.
      watch.mode='write'; watch.offset=0;
      watch.crc=options.crc32(watch.bytes);
      watch.phoneSave=phone;
      request({PB_CMD:CMD.BEGIN,PB_ROM_ID:watch.id,PB_SIZE:watch.bytes.length,
        PB_CRC32:watch.crc,PB_SAVE_TIME:timeBytes(phone.updatedAt)});
      return;
    }
    if (watchSave && (!phone || watchSave.updatedAt>phone.updatedAt || watchSave.data!==phone.data)) {
      replace(all,watch.id,watchSave);
      write(all);
    }
    finish('Saves synced.');
  }
  function handle(p) {
    if (!session || (p.PB_BANK>>>0)!==session.token) return;
    try {
      if (p.PB_CMD===CMD.ERROR) {finish(p.PB_TITLE || 'Save sync failed','Watch rejected sync');return;}
      if (session.mode==='read') {
        if (p.PB_CMD===CMD.INFO && session.message.PB_CMD===CMD.REQUEST) {
          if (!validId(p.PB_ROM_ID) || !validSize(p.PB_SIZE)) throw Error('Unsupported watch save');
          session.id=p.PB_ROM_ID;
          session.ramSize=p.PB_SIZE;
          session.updatedAt=readTime(p.PB_SAVE_TIME);
          session.filled=p.PB_SAVE_FILLED===1;
          console.log('pebbleboy saves: watch '+session.id+' filled='+session.filled+
            ' actionTime='+session.updatedAt+' ('+(session.updatedAt ?
              new Date(session.updatedAt).toISOString() : 'legacy/unknown')+')');
          session.crc=p.PB_CRC32>>>0;
          var all=library();
          var phone=all.games[session.id] && all.games[session.id].save;
          if ((!session.filled && !phone) || !session.ramSize ||
              (phone && session.filled && session.updatedAt>0 &&
               phone.updatedAt===session.updatedAt && (phone.origin!=='import' || phone.confirmed))) {
            if (phone) savedBytes(phone);
            finish('Saves synced.');
            return;
          }
          session.bytes=new Uint8Array(p.PB_SIZE+24);
          // An empty phone slot stays empty until the game writes SRAM or a .sav is imported.
          request({PB_CMD:CMD.READ,PB_OFFSET:0});
        } else if (p.PB_CMD===CMD.DATA && session.message.PB_CMD===CMD.READ) {
          if (p.PB_OFFSET!==session.offset) return;
          var expected=session.offset===session.ramSize ? 24 : 256;
          if (!p.PB_DATA || p.PB_DATA.length!==expected || (p.PB_CRC32>>>0)!==session.crc || p.PB_SIZE!==session.ramSize) throw Error('Invalid save chunk');
          session.bytes.set(p.PB_DATA,session.offset);
          session.offset+=p.PB_DATA.length;
          if (session.offset<session.bytes.length) request({PB_CMD:CMD.READ,PB_OFFSET:session.offset});
          else {
            if (options.crc32(session.bytes)!==session.crc) throw Error('Save checksum mismatch');
            reconcile();
          }
        }
      } else if (p.PB_CMD===CMD.ACK) {
        if (session.message.PB_CMD===CMD.COMMIT) {
          if (p.PB_OFFSET!==session.bytes.length || (p.PB_CRC32>>>0)!==session.crc) return;
          // Keep the action time unchanged when confirming delivery.
          var all=library();
          var delivered=all.games[session.id] && all.games[session.id].save;
          if (delivered && delivered.updatedAt===session.phoneSave.updatedAt && delivered.data===session.phoneSave.data) {
            delivered.confirmed=true;
            write(all);
          }
          finish('Save loaded on watch.');
        } else {
          var target=session.message.PB_CMD===CMD.BEGIN ? 0 : session.message.PB_OFFSET+session.message.PB_DATA.length;
          if (p.PB_OFFSET!==target) return;
          session.offset=target;
          writeNext();
        }
      }
    } catch(err) {finish(String(err.message || err),'Sync failed');}
  }
  function describe(includeData) {
    try {
      var all=library(), entries={};
      options.roms().forEach(function(entry) {
        var save=entrySave(all,entry), binding=bound(all,entry);
        entries[entry.id]={url:entry.url,filled:!!save,updatedAt:save ? save.updatedAt : 0,
          ramSize:save ? save.ramSize : (binding ? binding.ramSize : null)};
        if (includeData && save) {savedBytes(save);entries[entry.id].data=save.data;}
      });
      return {version:4,entries:entries,status:localStorage.getItem(STATUS) || '',busy:!!session};
    } catch(err) {return {version:4,entries:{},status:String(err.message || err)};}
  }
  function action(value) {
    try {
      if (value.action==='settings') {options.openPage();return;}
      var entry=romEntry(value.entryId), all=library();
      if (value.action==='import') {
        if (session) finish('', 'Superseded by import');
        var binding=bound(all,entry);
        var bytes=decode(value.data,binding ? binding.ramSize : null);
        var old=entrySave(all,entry);
        var save={data:options.toBase64(bytes),ramSize:bytes.length,crc32:options.crc32(bytes),
          updatedAt:Math.max(Date.now(),old ? old.updatedAt+1 : 1),origin:'import'};
        if (binding) replace(all,binding.romId,save);
        else all.pending[entry.id]={url:entry.url,save:save};
        write(all);
        status('Imported. Ready to sync.');
        options.openPage();
        if (options.idle()) sync(null,true);
      } else if (value.action==='export') {
        var current=entrySave(all,entry);
        if (!current) throw Error('This save slot is empty');
        savedBytes(current);
        options.openPage({exportSave:{entryId:entry.id,title:entry.name,
          ramSize:current.ramSize,data:current.data}});
      } else throw Error('Unknown save action');
    } catch(err) {status(String(err.message || err));options.openPage();}
  }
  function poll() {if (!session && options.idle()) sync();}
  function settingsPage() {
    // Refresh when settings opens, but let an offline phone show its local copy.
    if (session || !options.idle()) {options.openPage();return;}
    sync(function(){options.openPage();},true);
  }
  return {handle:handle,describe:describe,action:action,poll:poll,bind:bind,
    cancel:function(){if(session) finish("", "ROM changed");},
    romReady:function(){stop();sync(null,true);},
    settingsPage:settingsPage,busy:function(){return !!session;}};
};
