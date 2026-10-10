async function loadDocument() {
  const status = document.getElementById('docStatus');
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 10000);
  try {
    const isPlan = new URLSearchParams(location.search).get('doc') === 'futures';
    const download = document.getElementById('downloadDoc');
    download.href = isPlan ? '/futures.md' : '/docs.md';
    download.download = isPlan ? 'FUTURES_PLAN.md' : 'USER_GUIDE.md';
    document.title = isPlan ? '期貨提醒規劃' : 'ESP32 使用說明';
    const response = await fetch(isPlan ? '/futures.rendered' : '/docs.rendered', {cache:'no-store', signal:controller.signal});
    if (!response.ok) throw Error('HTTP ' + response.status);
    const article = document.getElementById('document');
    article.innerHTML = await response.text();
    article.querySelectorAll('a[href]').forEach(link => {
      if (link.protocol === 'http:' || link.protocol === 'https:') link.rel = 'noopener noreferrer';
    });
    status.textContent = '內建說明 · 無需網際網路即可閱讀';
  } catch (error) { status.textContent = '無法載入說明，請重新整理，或點選「下載 Markdown 原文」。'; }
  finally { clearTimeout(timer); }
}
loadDocument();
