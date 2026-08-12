function hmGetCookie(name){
  var prefix=name+'=',cookies=document.cookie.split(';');
  for(var i=0;i<cookies.length;i++){
    var value=cookies[i].trim();
    if(value.indexOf(prefix)===0)return value.substring(prefix.length);
  }
  return null;
}
function hmEscape(value){return String(value===undefined||value===null?'':value).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]})}
function hmPad(value){return String(value).padStart(2,'0')}
(function(){
  var darkMode=hmGetCookie('darkMode');
  var wantDark = darkMode==='true' || (darkMode===null && window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
  if(wantDark){
    document.documentElement.classList.add('dark-mode-loading');
  }
})();

function toggleMenu(){
  var m=document.getElementById('sideMenu');
  var o=document.getElementById('menuOverlay');
  m.classList.toggle('open');
  o.classList.toggle('open');
}
function closeMenu(){
  document.getElementById('sideMenu').classList.remove('open');
  document.getElementById('menuOverlay').classList.remove('open');
}

function setCookie(name, value, days) {
  var expires = "";
  if (days) {
    var date = new Date();
    date.setTime(date.getTime() + (days * 24 * 60 * 60 * 1000));
    expires = "; expires=" + date.toUTCString();
  }
  document.cookie = name + "=" + (value || "") + expires + "; path=/";
}

function toggleDarkMode() {
  var toggle = document.getElementById('darkModeToggle');
  var html = document.documentElement;
  
  if (toggle.checked) {
    html.classList.add('dark-mode');
    setCookie('darkMode', 'true', 365);
  } else {
    html.classList.remove('dark-mode');
    setCookie('darkMode', 'false', 365);
  }
}

function initDarkMode() {
  var darkMode = hmGetCookie('darkMode');
  var toggle = document.getElementById('darkModeToggle');
  var html = document.documentElement;

  // Remove the temporary loading class
  html.classList.remove('dark-mode-loading');

  // No explicit preference saved yet: follow the OS/browser setting
  var useDark;
  if (darkMode === 'true') {
    useDark = true;
  } else if (darkMode === 'false') {
    useDark = false;
  } else {
    useDark = !!(window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
  }

  if (useDark) {
    html.classList.add('dark-mode');
    if (toggle) toggle.checked = true;
  } else {
    html.classList.remove('dark-mode');
    if (toggle) toggle.checked = false;
  }
}

function markActiveNav() {
  var nav = document.getElementById('sideNav');
  if (!nav) return;
  var current = window.location.pathname || '/';
  nav.querySelectorAll('a[href]').forEach(function(link) {
    var target = link.getAttribute('href');
    if (!target || target.charAt(0) !== '/') return;
    link.classList.toggle('active', target === current);
  });
  var group = nav.querySelector('[data-custom-nav]');
  if (group) {
    var customPaths = ['/dashboard','/wpsettings','/scheduler','/externalsensors','/smartdhw','/hardware','/diagnostics','/history'];
    var customActive = customPaths.indexOf(current) >= 0;
    var toggle = group.querySelector('.sidemenu-group-toggle');
    group.classList.toggle('open', customActive || group.classList.contains('user-open'));
    if (toggle) {
      toggle.classList.toggle('active', customActive);
      toggle.setAttribute('aria-expanded', group.classList.contains('open') ? 'true' : 'false');
    }
  }
}

function createNavLink(href, label, icon, className) {
  var link = document.createElement('a');
  link.href = href;
  if (className) link.className = className;
  var iconElement = document.createElement('span');
  iconElement.className = 'nav-icon';
  iconElement.textContent = icon;
  link.appendChild(iconElement);
  link.appendChild(document.createTextNode(' ' + label));
  if (href === '/reboot') link.onclick = function() { return window.confirm('Reboot the device?'); };
  return link;
}

function groupCustomNav() {
  var nav = document.getElementById('sideNav');
  if (!nav || nav.querySelector('[data-custom-nav]')) return;
  var group = document.createElement('div');
  group.className = 'sidemenu-group';
  group.setAttribute('data-custom-nav', 'true');
  var toggle = document.createElement('button');
  toggle.type = 'button';
  toggle.className = 'sidemenu-group-toggle';
  toggle.setAttribute('aria-expanded', 'false');
  var icon = document.createElement('span');
  icon.className = 'nav-icon';
  icon.textContent = '◆';
  toggle.appendChild(icon);
  toggle.appendChild(document.createTextNode(' Custom Features'));
  var chevron = document.createElement('span');
  chevron.className = 'nav-chevron';
  chevron.textContent = '›';
  toggle.appendChild(chevron);
  toggle.onclick = function() {
    group.classList.toggle('user-open');
    group.classList.toggle('open');
    toggle.setAttribute('aria-expanded', group.classList.contains('open') ? 'true' : 'false');
  };
  group.appendChild(toggle);
  var submenu = document.createElement('div');
  submenu.className = 'sidemenu-submenu';
  submenu.appendChild(createNavLink('/dashboard', 'Dashboard', '▣'));
  submenu.appendChild(createNavLink('/wpsettings', 'WP Configuration', '⚙'));
  submenu.appendChild(createNavLink('/scheduler', 'Scheduler', '◷'));
  submenu.appendChild(createNavLink('/externalsensors', 'External Sensors', '◉'));
  submenu.appendChild(createNavLink('/smartdhw', 'Smart DHW', '♨'));
  submenu.appendChild(createNavLink('/diagnostics', 'Diagnostics', '◌'));
  submenu.appendChild(createNavLink('/history', 'History', '▥'));
  group.appendChild(submenu);
  nav.innerHTML = '';
  nav.appendChild(createNavLink('/', 'Home', '↳'));
  nav.appendChild(group);
  nav.appendChild(createNavLink('/firmware', 'Firmware', '⇧'));
  nav.appendChild(createNavLink('/reboot', 'Reboot', '↻', 'danger'));
  nav.appendChild(createNavLink('/rules', 'Rules', '⌘'));
  nav.appendChild(createNavLink('/settings', 'Network & System', '⚙'));
  markActiveNav();
}

function initActiveNav() {
  var nav = document.getElementById('sideNav');
  if (!nav) return;
  groupCustomNav();
  if (window.MutationObserver) {
    new MutationObserver(function() {
      if (!nav.querySelector('[data-custom-nav]')) groupCustomNav();
      else markActiveNav();
    }).observe(nav, {childList:true, subtree:true});
  }
}

// Initialize immediately when DOM is ready
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initDarkMode);
  document.addEventListener('DOMContentLoaded', initActiveNav);
} else {
  initDarkMode();
  initActiveNav();
}
