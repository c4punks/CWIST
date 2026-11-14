export function readFile() {
    return Promise.reject(new Error('Node filesystem access is unavailable in browser builds'));
}
