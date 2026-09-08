/* ROM entries and their single active save. No save bytes are sent over HTTP. */
(function() {
  'use strict';
  var MAX_SAV_BYTES=32768, MAX_ROMS=12;
  var cfg={};
  try {cfg=JSON.parse(decodeURIComponent(location.hash.slice(1))) || {};} catch(_) {}
  if(history.replaceState) history.replaceState(null,'',location.pathname+location.search);
  var externalExport=!!cfg.exportSave && !cfg.saveManager;
  var manager=cfg.saveManager || {}, support=manager.version===4;
  var rows=[], list=document.getElementById('roms');
  var feedback=document.getElementById('feedback');
  var nextId=0;
  function say(text) {feedback.textContent=text;}
  function node(tag,text,parent) {
    var el=document.createElement(tag);
    if(text) el.textContent=text;
    if(parent) parent.appendChild(el);
    return el;
  }
  function button(label,parent,callback) {
    var b=node('button',label,parent);b.type='button';b.addEventListener('click',callback);return b;
  }
  function settings() {
    var roms=[];
    rows.forEach(function(row) {
      var url=row.url.value.trim();
      if(url) roms.push({id:row.id,name:row.name.value.trim(),url:url,saveSlot:row.hasSlot});
    });
    return {roms:roms,scaleMode:document.getElementById('scale').value,
      audio:document.getElementById('audio').checked,clear:document.getElementById('clear').checked};
  }
  function close(value) {location.href='pebblejs://close#'+encodeURIComponent(JSON.stringify(value));}
  function action(value) {
    var current=settings();current.saveAction=value;close(current);
  }
  function encode(bytes) {
    var binary='';for(var i=0;i<bytes.length;i++) binary+=String.fromCharCode(bytes[i]);return btoa(binary);
  }
  function info(row) {
    var entry=manager.entries && manager.entries[row.id];
    return entry && entry.url===row.url.value.trim() ? entry : {filled:false,ramSize:null};
  }
  function renderSlot(row) {
    row.slot.textContent='';
    if(!row.hasSlot) {
      var add=button('Add save slot',row.slot,function(){row.hasSlot=true;renderSlot(row);});
      add.disabled=!support;
      return;
    }
    var state=info(row);
    var label=state.filled ? 'Updated '+(state.updatedAt ? new Date(state.updatedAt).toLocaleString() : 'previously') : 'Empty save slot';
    node('p',label,row.slot).className='hint';
    var actions=node('div','',row.slot);actions.className='actions';
    var file=node('input','',row.slot);file.type='file';file.accept='.sav';file.hidden=true;
    file.setAttribute('aria-label','Import save for '+(row.name.value || 'this game'));
    var upload=button('Import .sav',actions,function(){
      if(!row.url.value.trim()) {say('Add the ROM URL first.');return;}
      file.click();
    });
    upload.disabled=!support || state.ramSize===0;
    var download=button('Export .sav',actions,function(){
      var current=info(row);
      showExport({title:row.name.value,ramSize:current.ramSize,data:current.data});
    });
    download.disabled=!support || !state.filled;
    file.addEventListener('change',function(){
      var selected=file.files && file.files[0];if(!selected) return;
      if(!/\.sav$/i.test(selected.name) || !selected.size || selected.size>MAX_SAV_BYTES) {
        say('Choose a non-empty .sav up to 32 KiB.');file.value='';return;
      }
      var expected=info(row).ramSize;
      if([512,2048,8192,32768].indexOf(selected.size)<0 || (expected!=null && selected.size!==expected)) {
        say(expected!=null ? 'Expected a '+expected+'-byte .sav for this ROM.' : 'Unsupported .sav size.');file.value='';return;
      }
      if(info(row).filled && !confirm('Replace the save for '+(row.name.value || 'this game')+'? It will sync to the watch when this game is open.')) {
        file.value='';return;
      }
      var reader=new FileReader();
      reader.onerror=function(){say('Could not read this file.');file.value='';};
      reader.onload=function(){
        try {
          var bytes=new Uint8Array(reader.result);
          if(bytes.length!==selected.size) throw Error('File size changed while reading');
          // File lastModified is deliberately ignored. The companion timestamps import.
          action({action:'import',entryId:row.id,data:encode(bytes)});
        } catch(e) {say(e.message);}
      };
      reader.readAsArrayBuffer(selected);
    });
  }
  function addRom(item) {
    if(rows.length>=MAX_ROMS) return;
    item=item || {saveSlot:false};
    var card=node('div','',list);card.className='rom';
    var row={id:item.id || ('entry-'+Date.now().toString(36)+'-'+(++nextId)),card:card,hasSlot:item.saveSlot!==false};
    row.name=node('input','',card);row.name.type='text';row.name.maxLength=32;row.name.placeholder='Game name';row.name.value=item.name || '';
    row.name.setAttribute('aria-label','Game name');
    row.url=node('input','',card);row.url.type='url';row.url.maxLength=2048;row.url.placeholder='ROM download URL';row.url.value=item.url || '';
    row.url.setAttribute('aria-label','ROM download URL');
    row.slot=node('div','',card);row.slot.className='slot';
    var remove=button('Remove ROM',card,function(){rows.splice(rows.indexOf(row),1);card.remove();});remove.className='remove';
    row.url.addEventListener('input',function(){renderSlot(row);});
    rows.push(row);renderSlot(row);
  }
  document.getElementById('scale').value=cfg.scaleMode==='fullscreen' || cfg.scaleMode==='fit' ? cfg.scaleMode : '1x';
  document.getElementById('audio').checked=cfg.audio===true;
  (cfg.roms || []).slice(0,MAX_ROMS).forEach(addRom);
  if(!rows.length) addRom();
  document.getElementById('add').addEventListener('click',function(){addRom();});
  document.getElementById('save').addEventListener('click',function(){
    var value=settings();
    if(!value.roms.length) {say('Add a ROM URL first.');return;}
    value.launch=true;close(value);
  });
  say(manager.status || '');
  var exportUrl=null;
  function showExport(save) {
    document.getElementById('settings-fields').hidden=true;
    var area=document.getElementById('export');area.hidden=false;area.textContent='';
    if(exportUrl) URL.revokeObjectURL(exportUrl);
    try {
      if(!save.ramSize || save.ramSize>MAX_SAV_BYTES || typeof save.data!=='string' || save.data.length>Math.ceil(MAX_SAV_BYTES/3)*4) throw Error('Invalid save');
      var binary=atob(save.data), bytes=new Uint8Array(binary.length);
      if(bytes.length!==save.ramSize) throw Error('Invalid save size');
      for(var i=0;i<bytes.length;i++) bytes[i]=binary.charCodeAt(i);
      node('h2',save.title,area);
      var name=String(save.title || 'game').replace(/[^A-Za-z0-9_-]+/g,'-')+'.sav';
      if(externalExport) {
        var blob=new Blob([bytes],{type:'application/octet-stream'});
        var link=node('a','Download .sav',area);link.className='download';link.download=name;link.href=exportUrl=URL.createObjectURL(blob);
        return;
      }
      // Embedded WebViews may not implement file downloads. A browser can save the
      // same bytes; the fragment never goes to the web server.
      var browserUrl=location.origin+location.pathname+location.search+'#'+
        encodeURIComponent(JSON.stringify({exportSave:save}));
      button('Copy browser download link',area,function(){
        function fallback() {
          var text=node('textarea','',area);text.value=browserUrl;text.readOnly=true;
          text.style.width='100%';text.setAttribute('aria-label','Browser download link');
          text.select();text.setSelectionRange(0,text.value.length);
          var copied=false;try {copied=document.execCommand('copy');} catch (_) {}
          say(copied ? 'Link copied. Paste it into your browser to download.' : 'Copy the selected link and paste it into your browser.');
          if(copied) text.remove();
        }
        if(navigator.clipboard && navigator.clipboard.writeText) {
          navigator.clipboard.writeText(browserUrl).then(function(){
            say('Link copied. Paste it into your browser to download.');
          },fallback);
        } else fallback();
      });
      node('p','Paste the link into Chrome or Safari, then tap Download .sav.',area).className='hint';
      button('Back to settings',area,function(){
        area.hidden=true;document.getElementById('settings-fields').hidden=false;
      });
    } catch(e) {say(e.message);}
  }
  if(cfg.exportSave) showExport(cfg.exportSave);
}());
