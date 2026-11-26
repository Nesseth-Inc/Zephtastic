async page => {
  const form = page.getByRole('button',{name:'Send message'});
  const text = page.getByRole('textbox',{name:'Message'});
  const dest = page.getByRole('textbox',{name:'Recipient'});
  await page.getByText('Connected',{exact:true}).waitFor();
  await page.locator('#messages li').first().waitFor();
  await page.getByRole('combobox',{name:'Channel'}).selectOption('3');
  await text.fill('Synthetic café message');
  await form.click();
  await page.getByText('Radio acknowledged',{exact:true}).first().waitFor();
  await dest.fill('!1234abcd');
  await text.fill('Direct fixture 🛰');
  await form.click();
  await page.waitForFunction(() => [...document.querySelectorAll('#messages li small')].filter(x => x.textContent==='Radio acknowledged').length===2);
  await dest.fill('invalid');
  await text.fill('validation');
  await form.click();
  await page.getByText('Enter an eight-digit hexadecimal node ID.').waitFor();
  await text.fill('🛰'.repeat(59));
  if (!await form.isDisabled()) throw Error('Oversized UTF-8 message permitted');
  if (await page.locator('#messages li').count()!==3) throw Error('Duplicate or missing on-screen messages');
  if (await page.locator('#messages p b').count()) throw Error('Received markup parsed as HTML');
  if (await page.locator('#messages p').first().textContent()!=='<b>Signal</b> Hello from the mesh') throw Error('Literal text mismatch');
  await dest.fill('');
  await text.fill('');
  await page.setViewportSize({width:390,height:844});
  return {status:'PASS',sent:2,history:3,width:await page.evaluate(()=>document.documentElement.scrollWidth)};
}
