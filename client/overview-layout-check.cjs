const { chromium } = require(''@playwright/test'');

(async () => {
  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  await page.goto(''http://127.0.0.1:3000/overview'', { waitUntil: ''networkidle'' });
  await page.waitForSelector(''text=תצוגת צומת בזמן אמת'', { timeout: 15000 });

  const eastVisible = await page.evaluate(() => {
    const nodes = [...document.querySelectorAll(''.lane-header strong'')];
    return nodes.some((n) => (n.textContent || '''').trim() === ''מזרח'');
  });

  const westVisible = await page.evaluate(() => {
    const nodes = [...document.querySelectorAll(''.lane-header strong'')];
    return nodes.some((n) => (n.textContent || '''').trim() === ''מערב'');
  });

  const result = { eastVisible, westVisible };
  console.log(JSON.stringify(result, null, 2));
  await browser.close();
})();
