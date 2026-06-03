const { chromium } = require('@playwright/test');

(async () => {
  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage();
  const httpUrls = [];
  const wsUrls = [];
  const failures = [];

  page.on('request', (req) => {
    const url = req.url();
    if (url.includes('127.0.0.1:9000')) httpUrls.push(url);
  });
  page.on('requestfailed', (req) => failures.push({ url: req.url(), error: req.failure()?.errorText || 'failed' }));
  page.on('websocket', (ws) => wsUrls.push(ws.url()));

  await page.goto('http://127.0.0.1:3000/overview', { waitUntil: 'networkidle' });
  await page.waitForSelector('text=מצב תנועה בצומת', { timeout: 15000 });
  const overviewText = await page.locator('body').innerText();

  await page.goto('http://127.0.0.1:3000/admin', { waitUntil: 'networkidle' });
  await page.getByPlaceholder('שם משתמש').fill('admin');
  await page.getByPlaceholder('סיסמה').fill('TrafficAdmin123');
  await page.getByRole('button', { name: 'התחבר כמנהל' }).click();
  await page.waitForSelector('text=מחובר כעת ומאומת מול השרת.', { timeout: 15000 });
  const adminText = await page.locator('body').innerText();

  const result = {
    overviewHasTrafficCard: overviewText.includes('מצב תנועה בצומת'),
    overviewHasPhase0: overviewText.includes('Phase0'),
    overviewHasQueue26: overviewText.includes('26'),
    adminLoginSucceeded: adminText.includes('מחובר כעת ומאומת מול השרת.'),
    hitHttp9000: [...new Set(httpUrls.filter((u) => u.includes('127.0.0.1:9000')))],
    hitWs9001: [...new Set(wsUrls.filter((u) => u.includes('127.0.0.1:9001')))],
    failures,
  };

  console.log(JSON.stringify(result, null, 2));
  await browser.close();
})();
