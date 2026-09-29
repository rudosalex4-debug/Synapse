export interface MaxWebApp {initData?:string;platform?:string;version?:string;ready?:()=>void;enableClosingConfirmation?:()=>void;disableClosingConfirmation?:()=>void}
declare global {interface Window {WebApp?:MaxWebApp}}
export type BridgeState='loading'|'loaded'|'unavailable';
export interface BridgeSummary {state:BridgeState;platform:string;hasLaunchData:boolean}
export function bridgeSummary(app:MaxWebApp|undefined,state:BridgeState):BridgeSummary {
  const labels:Record<string,string>={android:'Android',ios:'iOS',web:'MAX Web',desktop:'MAX Desktop'};
  return {state,platform:app?.platform?(labels[app.platform.toLowerCase()]??'Другой клиент'):'Браузер / не определён',hasLaunchData:typeof app?.initData==='string'&&app.initData.length>0};
}
let loading:Promise<MaxWebApp|undefined>|undefined;
export function loadBridge():Promise<MaxWebApp|undefined> {
  if(window.WebApp) return Promise.resolve(window.WebApp);
  if(loading) return loading;
  const attempt=new Promise<MaxWebApp|undefined>(resolve=>{
    const script=document.createElement('script');script.src='https://st.max.ru/js/max-web-app.js';script.async=true;
    let complete=false;
    const finish=()=>{
      if(complete)return;
      complete=true;window.clearTimeout(timer);script.onload=null;script.onerror=null;
      const app=window.WebApp;
      if(!app)script.remove();
      resolve(app);
    };
    const timer=window.setTimeout(finish,6000);script.onload=finish;script.onerror=finish;
    try {document.head.append(script)} catch {finish()}
  }).catch(()=>undefined);
  loading=attempt;
  // Share only the in-flight attempt. A failed load must allow an explicit retry.
  void attempt.then(()=>{if(loading===attempt)loading=undefined});
  return attempt;
}
