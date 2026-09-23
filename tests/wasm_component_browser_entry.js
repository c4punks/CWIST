/* Browser-bundle entry for `make component-browser-smoke`.
 *
 * Copied into .jco-guest/<target>/ before bundling; esbuild resolves
 * 'cwist-guest-component' to the --no-nodejs-compat re-transpile via the
 * Makefile's --alias flag. The bundle is a build-time packaging gate and is
 * not executed: its loader fetches the wasm shards the browser way. */
import { guest, run } from 'cwist-guest-component';
import { createCwistFromComponent } from '../../wasm/npm/component.js';

await run.run();

const handle = createCwistFromComponent({
  dispatch: guest.dispatch,
  useSession: guest.useSession,
});

const res = await handle({ method: 'GET', path: '/hello' });
if (res.status !== 200) throw new Error('status: ' + res.status);
const text = new TextDecoder().decode(res.body);
if (text !== 'hello-from-wrapper-test') {
  throw new Error('body: ' + JSON.stringify(text));
}

console.log('component-browser-smoke: PASS (browser bundle served /hello)');
