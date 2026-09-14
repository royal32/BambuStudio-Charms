//var TestData={"sequence_id":"0","command":"get_recent_projects","response":[{"path":"D:\\work\\Models\\Toy\\3d-puzzle-cube-model_files\\3d-puzzle-cube.3mf","time":"2022\/3\/24 20:33:10"},{"path":"D:\\work\\Models\\Art\\Carved Stone Vase - remeshed+drainage\\Carved Stone Vase.3mf","time":"2022\/3\/24 17:11:51"},{"path":"D:\\work\\Models\\Art\\Kity & Cat\\Cat.3mf","time":"2022\/3\/24 17:07:55"},{"path":"D:\\work\\Models\\Toy\\鐩村墤.3mf","time":"2022\/3\/24 17:06:02"},{"path":"D:\\work\\Models\\Toy\\minimalistic-dual-tone-whistle-model_files\\minimalistic-dual-tone-whistle.3mf","time":"2022\/3\/22 21:12:22"},{"path":"D:\\work\\Models\\Toy\\spiral-city-model_files\\spiral-city.3mf","time":"2022\/3\/22 18:58:37"},{"path":"D:\\work\\Models\\Toy\\impossible-dovetail-puzzle-box-model_files\\impossible-dovetail-puzzle-box.3mf","time":"2022\/3\/22 20:08:40"}]};

var m_HotModelList=null;
var m_HasNetworkPlugin=true;
var m_GetPrintHistoryStatus=false;
var m_IsUserLogin=false;
var m_ServerConnectFailed=false;
var m_AccountProfiles=[];
var m_ActiveProfileId='';

function OnInit()
{
	//-----Official-----
    TranslatePage();

	UpdateServerConnectFailTipVisible();
	SendMsg_GetLoginInfo();
	SendMsg_GetAccountProfiles();
	GotoMenu( 'home' );
	$("#Login2").click(function() {
		$("#DropdownWrapper").css("visibility", "visible");
	});
	$(document).on('click', function(e) {
		if (!$(e.target).closest('#Login2').length) {
			$("#DropdownWrapper").css("visibility", "hidden");
		}
	});
	$('#Login2').on('focusout', function(event) {
		// Keep the menu open when focus moves to a saved-account button.
		if(!this.contains(event.relatedTarget))
			$("#DropdownWrapper").css("visibility", "hidden");
	});
	OnUpdatePluginInstalltip();
}

function HandleStudio( pVal )
{
	let strCmd = pVal['command'];

	if(strCmd=='studio_userlogin')
	{
		var lastLoginInfo = pVal;
		m_IsUserLogin=true;
		SetLoginInfo(pVal['data']['avatar'],pVal['data']['name']);
		UpdateServerConnectFailTipVisible();
		if (!m_GetPrintHistoryStatus && (pVal != lastLoginInfo)) {
			SendMsg_GetPrintHistory();
		}
	}
	else if(strCmd=='studio_useroffline')
	{
		m_IsUserLogin=false;
		m_ServerConnectFailed=false;
		SetUserOffline();
		UpdateServerConnectFailTipVisible();
		m_GetPrintHistoryStatus=false;
	}
	else if(strCmd=='homepage_server_connect_status')
	{
		m_ServerConnectFailed=(pVal['failed']*1)==1;
		UpdateServerConnectFailTipVisible();
	}
	else if(strCmd=='studio_account_profiles')
	{
		SetAccountProfiles(pVal['data'] || {});
	}
	else if(strCmd=='studio_account_switching')
	{
		$("#AccountSwitchingOverlay").css("display", pVal['active']===false ? "none" : "flex");
	}
	else if( strCmd=="network_plugin_installtip" )
	{
		let nShow=pVal["show"]*1;

	    if(nShow==1)
		{
			$("#NoPluginTip").show();
			$("#NoPluginTip").css("display","flex");

			m_HasNetworkPlugin=false;
		}
		else
		{
			$("#NoPluginTip").hide();
			m_HasNetworkPlugin=true;
		}
	}
	else if(strCmd=='homepage_leftmenu_clicked')
	{
		let NewMenu=pVal['menu'];
		//alert('LeftMenu Clicked:'+strMenu );

		GotoMenu(NewMenu);
	}
	else if(strCmd=='homepage_leftmenu_newtag')
	{
		let NewMenu=pVal['menu'];
		let nShow=pVal['show'];

		ShowMenuNewTag(NewMenu,nShow);
	}
	else if(strCmd=='homepage_leftmenu_show')
	{
		let NewMenu=pVal['menu'];
		let nShow=pVal['show'];

		ShowMenuBtn(NewMenu,nShow);
	}
	else if(strCmd=='printhistory_task_show')
	{
		m_GetPrintHistoryStatus=true;
	}
}

function UpdateServerConnectFailTipVisible()
{
	if(m_IsUserLogin && m_ServerConnectFailed)
	{
		$("#LoginArea").addClass("ServerConnectFailVisible");
		$("#ServerConnectFailTip").show();
		$("#ServerConnectFailTip").css("display","flex");
	}
	else
	{
		$("#LoginArea").removeClass("ServerConnectFailVisible");
		$("#ServerConnectFailTip").hide();
	}
}

var NowMenu='';
function GotoMenu( strMenu )
{
	ShowMenuNewTag(strMenu,0);

	if(NowMenu==strMenu && strMenu!='makersupply')
		return;

	NowMenu=strMenu;

	let MenuList=$(".BtnItem");
	let nAll=MenuList.length;

	for(let n=0;n<nAll;n++)
	{
		let OneBtn=MenuList[n];

		if( $(OneBtn).attr("menu")==strMenu )
		{
			if(strMenu!=='makersupply')
			{
				$(".BtnItem").removeClass("BtnItemSelected");
				$(OneBtn).addClass("BtnItemSelected");
			}

			//SendWX
			var tSend={};
			tSend['sequence_id']=Math.round(new Date() / 1000);
			tSend['command']="homepage_leftmenu_clicked";
			tSend['menu']=strMenu;
			tSend['refresh']=0;

			SendWXMessage( JSON.stringify(tSend) );
		}
	}
}

function OnManualExternalLinkClick(evt)
{
	// Keep the row click (GotoMenu) for the in-app wiki view; this icon alone
	// jumps to the same-region landing page in the system browser, mirroring
	// wiki.html's own openAcademyUrl() region split (mainland goes to
	// bambulab.cn, wiki.bambulab.com does not resolve correctly there).
	if(evt && evt.stopPropagation) evt.stopPropagation();

	let strRegion=GetQueryString("region");

	let open_url;
	if(strRegion=="CN")
	{
		open_url="https://bambulab.cn/zh-cn/support/academy/";
	}
	else
	{
		let strLang=GetQueryString("lang");
		if(strLang==null)
			strLang=localStorage.getItem(LANG_COOKIE_NAME);

		let lang;
		if(strLang!=null && strLang.includes('zh')) lang='zh';
		else if(strLang!=null && strLang.includes('fr')) lang='fr-fr';
		else if(strLang!=null && strLang.includes('de')) lang='de-de';
		else if(strLang!=null && strLang.includes('es')) lang='es-mx';
		else if(strLang!=null && strLang.includes('it')) lang='it-it';
		else if(strLang!=null && strLang.includes('ja')) lang='ja-jp';
		else if(strLang!=null && strLang.includes('ko')) lang='ko-kr';
		else if(strLang!=null && strLang.includes('pt')) lang='pt-br';
		else if(strLang!=null && strLang.includes('nl')) lang='nl-nl';
		else lang='en';

		open_url="https://bambulab.com/"+lang+"/support/academy/";
	}

	OpenUrlInLocalBrowser(open_url);
}

function ShowMenuNewTag(MenuName,nStatus)
{
	//alert(MenuName+" - "+nStatus);
	if(MenuName=='online')
	{
		if(nStatus==1)
			$('#OnlineNewTag').show();
		else
			$('#OnlineNewTag').hide();
	}
	else if(MenuName=='makerlab')
	{
		if(nStatus==1)
			$('#MakerlabNewTag').show();
		else
			$('#MakerlabNewTag').hide();
	}
}

function ShowMenuBtn( MenuName,nShow)
{
	let sKey='div[menu="'+MenuName+'"]';

	if(nShow==1)
		$(sKey).css('display','flex');
	else
		$(sKey).css('display','none');
}


function SetLoginInfo( strAvatar, strName )
{
	$("#Login1").hide();

	$("#UserName").text(strName);
	$("#DropdownUserName").text(strName);

	let safeAvatar=SafeAvatarUrl(strAvatar);
	let OriginAvatar=$("#UserAvatarIcon").prop("src");
	if(safeAvatar!=OriginAvatar) {
		$("#UserAvatarIcon").prop("src",safeAvatar || "img/left_home_account.svg");
		$("#DropdownAvatar").css("background-image", safeAvatar ? "url('"+safeAvatar+"')" : "url('../img/left_home_account.svg')");
	}else
	{
		//alert('Avatar is Same');
	}

	$("#Login2").show();
	$("#Login2").css("display","flex");
	$("#SignInFooter").hide();
	$("#LogoutFooter").show();
}

function SetUserOffline()
{
	m_IsUserLogin=false;
	$("#UserAvatarIcon").prop("src","img/left_home_account.svg");
	$("#DropdownAvatar").css("background-image","url('../img/left_home_account.svg')");
	$("#UserName").text('');
	$("#DropdownUserName").text('');
	UpdateAccountEntryVisibility();
}

function SetAccountProfiles(data)
{
	m_AccountProfiles=Array.isArray(data['accounts']) ? data['accounts'] : [];
	m_ActiveProfileId=data['active_id'] || '';
	m_IsUserLogin=!!data['logged_in'];
	$("#AccountList").toggle(m_AccountProfiles.length>0);
	$("#AddAccountFooter").toggle(m_AccountProfiles.length>0);
	RenderAccountProfiles();
	UpdateAccountEntryVisibility();
}

function GetActiveAccountProfile()
{
	for(let i=0;i<m_AccountProfiles.length;i++) {
		if(m_AccountProfiles[i]['id']===m_ActiveProfileId)
			return m_AccountProfiles[i];
	}
	return null;
}

function SafeAvatarUrl(url)
{
	return (typeof url==='string' && /^https?:\/\//i.test(url) && !/['"()\\\r\n]/.test(url)) ? url : '';
}

function UpdateAccountEntryVisibility()
{
	let active=GetActiveAccountProfile();
	let hasSavedAccount=active && ((active['user_name'] || '').length>0 || m_AccountProfiles.length>1);
	if(m_IsUserLogin || hasSavedAccount) {
		if(active) {
			$("#UserName").text(active['name'] || active['user_name'] || 'Bambu account');
			$("#DropdownUserName").text(active['name'] || active['user_name'] || 'Bambu account');
			let avatar=SafeAvatarUrl(active['avatar']);
			$("#UserAvatarIcon").prop("src", avatar || "img/left_home_account.svg");
			$("#DropdownAvatar").css("background-image", avatar ? "url('"+avatar+"')" : "url('../img/left_home_account.svg')");
		}
		$("#Login1").hide();
		$("#Login2").css("display","flex");
	} else {
		$("#Login2").hide();
		$("#Login1").css("display","flex");
	}
	$("#SignInFooter").toggle(!m_IsUserLogin);
	$("#LogoutFooter").toggle(m_IsUserLogin);
}

function RenderAccountProfiles()
{
	let list=$("#AccountList");
	list.empty();
	for(let i=0;i<m_AccountProfiles.length;i++) {
		let account=m_AccountProfiles[i];
		let row=$("<div>").addClass("AccountRow").attr({role:'button', tabindex:0});
		if(account['id']===m_ActiveProfileId)
			row.addClass("AccountRowActive");
		let avatar=$("<div>").addClass("AccountRowAvatar");
		let avatarUrl=SafeAvatarUrl(account['avatar']);
		if(avatarUrl)
			avatar.css("background-image", "url('"+avatarUrl+"')");
		let name=$("<div>").addClass("AccountRowName").text(account['name'] || account['user_name'] || 'Bambu account');
		row.attr('title', name.text() + (account['id']===m_ActiveProfileId ? '' : ' — Restart Studio to switch to this account'));
		let check=$("<div>").addClass("AccountRowCheck").text(account['id']===m_ActiveProfileId ? "✓" : "");
		row.append(avatar, name, check);
		row.on('click', function(event) {
			event.stopPropagation();
			if(account['id']===m_ActiveProfileId) {
				if(!m_IsUserLogin) OnLoginOrRegister();
				return;
			}
			OnSwitchAccount(account['id']);
		});
		row.on('keydown', function(event) {
			if(event.key==='Enter' || event.key===' ') { event.preventDefault(); row.trigger('click'); }
		});
		list.append(row);
	}
}

function SetMallUrl( strUrl )
{
	$("#MallWeb").prop("src",strUrl);
}

/*-------RecentFile MX Message------*/
function SendMsg_GetLoginInfo()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="get_login_info";

	SendWXMessage( JSON.stringify(tSend) );
}

function SendMsg_GetAccountProfiles()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']='get_account_profiles';
	SendWXMessage(JSON.stringify(tSend));
}

function OnSwitchAccount(profileId)
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']='homepage_switch_account';
	tSend['profile_id']=profileId;
	SendWXMessage(JSON.stringify(tSend));
}

function OnAddAccount(event)
{
	if(event) event.stopPropagation();
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']='homepage_add_account';
	SendWXMessage(JSON.stringify(tSend));
}

function OnLoginFromMenu(event)
{
	if(event) event.stopPropagation();
	OnLoginOrRegister();
}

function OnLoginOrRegister()
{
	var tSend={};

	if( m_HasNetworkPlugin )
	{
		tSend['sequence_id']=Math.round(new Date() / 1000);
		tSend['command']="homepage_login_or_register";
	}
	else
	{
		tSend['sequence_id']=Math.round(new Date() / 1000);
		tSend['command']="homepage_need_networkplugin";
	}

	SendWXMessage( JSON.stringify(tSend) );
}

function OnLogOut()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_logout";

	SendWXMessage( JSON.stringify(tSend) );
}

function OnUpdatePluginInstalltip()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="update_plugin_installtip";

	SendWXMessage( JSON.stringify(tSend) );
}

function SendMsg_CheckNewTag()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_leftmenu_newtag";

	SendWXMessage( JSON.stringify(tSend) );
}

function BeginDownloadNetworkPlugin()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="begin_network_plugin_download";

	SendWXMessage( JSON.stringify(tSend) );
}

function SendMsg_GetPrintHistory()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_printhistory_get";

	SendWXMessage( JSON.stringify(tSend) );
}

var WidthBoundary=168;
function ChangeLeftWidth()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_leftmenu_change_width";

	let NowWidth=window.innerWidth;
	if(NowWidth<=WidthBoundary)
	{
		tSend['width']=224;

		ShowLeftMenuTip( false );
	}
	else
	{
		tSend['width']=64;

		ShowLeftMenuTip( true );
	}

	SendWXMessage( JSON.stringify(tSend) );
}

function ShowLeftMenuTip( bShow )
{
	if(bShow==true)
	{
		$("div[menu='home'] div.BtnIcon").prop('title',GetCurrentTextByKey('t114'));
		$("div[menu='recent'] div.BtnIcon").prop('title',GetCurrentTextByKey('t35'));
		$("div[menu='online'] div.BtnIcon").prop('title',GetCurrentTextByKey('t107'));
		$("div[menu='makerlab'] div.BtnIcon").prop('title','MakerLab');
		$("div[menu='makersupply'] div.BtnIcon").prop('title',GetCurrentTextByKey('t125'));
		$("div[menu='printhistory'] div.BtnIcon").prop('title',GetCurrentTextByKey('t117'));
		$("div[menu='manual'] div.BtnIcon").prop('title',GetCurrentTextByKey('t87'));
	}
	else
	{
		$("div[menu='home'] div.BtnIcon").removeAttr('title');
		$("div[menu='recent'] div.BtnIcon").removeAttr('title');
		$("div[menu='online'] div.BtnIcon").removeAttr('title');
		$("div[menu='makerlab'] div.BtnIcon").removeAttr('title');
		$("div[menu='makersupply'] div.BtnIcon").removeAttr('title');
		$("div[menu='printhistory'] div.BtnIcon").removeAttr('title');
		$("div[menu='manual'] div.BtnIcon").removeAttr('title');
	}
}

//---------------Global-----------------
window.postMessage = HandleStudio;
