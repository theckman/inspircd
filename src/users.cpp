/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2009 InspIRCd Development Team
 * See: http://wiki.inspircd.org/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include <stdarg.h>
#include "socketengine.h"
#include "xline.h"
#include "bancache.h"
#include "commands/cmd_whowas.h"

typedef unsigned int uniq_id_t;
class sent
{
	uniq_id_t uniq_id;
	uniq_id_t* array;
	void init()
	{
		if (!array)
			array = new uniq_id_t[ServerInstance->SE->GetMaxFds()];
		memset(array, 0, ServerInstance->SE->GetMaxFds() * sizeof(uniq_id_t));
		uniq_id++;
	}
 public:
	sent() : uniq_id(static_cast<uniq_id_t>(-1)), array(NULL) {}
	inline uniq_id_t operator++()
	{
		if (++uniq_id == 0)
			init();
		return uniq_id;
	}
	inline uniq_id_t& operator[](int i)
	{
		return array[i];
	}
	~sent()
	{
		delete array;
	}
};

static sent already_sent;

std::string User::ProcessNoticeMasks(const char *sm)
{
	bool adding = true, oldadding = false;
	const char *c = sm;
	std::string output;

	while (c && *c)
	{
		switch (*c)
		{
			case '+':
				adding = true;
			break;
			case '-':
				adding = false;
			break;
			case '*':
				for (unsigned char d = 'A'; d <= 'z'; d++)
				{
					if (ServerInstance->SNO->IsEnabled(d))
					{
						if ((!IsNoticeMaskSet(d) && adding) || (IsNoticeMaskSet(d) && !adding))
						{
							if ((oldadding != adding) || (!output.length()))
								output += (adding ? '+' : '-');

							this->SetNoticeMask(d, adding);

							output += d;
						}
					}
					oldadding = adding;
				}
			break;
			default:
				if ((*c >= 'A') && (*c <= 'z') && (ServerInstance->SNO->IsEnabled(*c)))
				{
					if ((!IsNoticeMaskSet(*c) && adding) || (IsNoticeMaskSet(*c) && !adding))
					{
						if ((oldadding != adding) || (!output.length()))
							output += (adding ? '+' : '-');

						this->SetNoticeMask(*c, adding);

						output += *c;
					}
				}
				else
					this->WriteNumeric(ERR_UNKNOWNSNOMASK, "%s %c :is unknown snomask char to me", this->nick.c_str(), *c);

				oldadding = adding;
			break;
		}

		*c++;
	}

	std::string s = this->FormatNoticeMasks();
	if (s.length() == 0)
	{
		this->modes[UM_SNOMASK] = false;
	}

	return output;
}

void User::StartDNSLookup()
{
	try
	{
		bool cached = false;
		const char* sip = this->GetIPString();
		UserResolver *res_reverse;

		QueryType resolvtype = this->client_sa.sa.sa_family == AF_INET6 ? DNS_QUERY_PTR6 : DNS_QUERY_PTR4;
		res_reverse = new UserResolver(this, sip, resolvtype, cached);

		ServerInstance->AddResolver(res_reverse, cached);
	}
	catch (CoreException& e)
	{
		ServerInstance->Logs->Log("USERS", DEBUG,"Error in resolver: %s",e.GetReason());
	}
}

bool User::IsNoticeMaskSet(unsigned char sm)
{
	if (!isalpha(sm))
		return false;
	return (snomasks[sm-65]);
}

void User::SetNoticeMask(unsigned char sm, bool value)
{
	if (!isalpha(sm))
		return;
	snomasks[sm-65] = value;
}

const char* User::FormatNoticeMasks()
{
	static char data[MAXBUF];
	int offset = 0;

	for (int n = 0; n < 64; n++)
	{
		if (snomasks[n])
			data[offset++] = n+65;
	}

	data[offset] = 0;
	return data;
}

bool User::IsModeSet(unsigned char m)
{
	if (!isalpha(m))
		return false;
	return (modes[m-65]);
}

void User::SetMode(unsigned char m, bool value)
{
	if (!isalpha(m))
		return;
	modes[m-65] = value;
}

const char* User::FormatModes(bool showparameters)
{
	static char data[MAXBUF];
	std::string params;
	int offset = 0;

	for (unsigned char n = 0; n < 64; n++)
	{
		if (modes[n])
		{
			data[offset++] = n + 65;
			ModeHandler* mh = ServerInstance->Modes->FindMode(n + 65, MODETYPE_USER);
			if (showparameters && mh && mh->GetNumParams(true))
			{
				std::string p = mh->GetUserParameter(this);
				if (p.length())
					params.append(" ").append(p);
			}
		}
	}
	data[offset] = 0;
	strlcat(data, params.c_str(), MAXBUF);
	return data;
}

void User::DecrementModes()
{
	ServerInstance->Logs->Log("USERS", DEBUG, "DecrementModes()");
	for (unsigned char n = 'A'; n <= 'z'; n++)
	{
		if (modes[n-65])
		{
			ServerInstance->Logs->Log("USERS", DEBUG,"DecrementModes() found mode %c", n);
			ModeHandler* mh = ServerInstance->Modes->FindMode(n, MODETYPE_USER);
			if (mh)
			{
				ServerInstance->Logs->Log("USERS", DEBUG,"Found handler %c and call ChangeCount", n);
				mh->ChangeCount(-1);
			}
		}
	}
}

User::User(const std::string &uid)
{
	server = ServerInstance->Config->ServerName;
	age = ServerInstance->Time();
	Penalty = 0;
	lastping = signon = idle_lastmsg = nping = registered = 0;
	bytes_in = bytes_out = cmds_in = cmds_out = 0;
	quietquit = quitting = exempt = haspassed = dns_done = false;
	fd = -1;
	server_sa.sa.sa_family = AF_UNSPEC;
	client_sa.sa.sa_family = AF_UNSPEC;
	MyClass = NULL;
	AllowedPrivs = AllowedOperCommands = NULL;

	if (uid.empty())
		uuid.assign(ServerInstance->GetUID(), 0, UUID_LENGTH - 1);
	else
		uuid.assign(uid, 0, UUID_LENGTH - 1);

	ServerInstance->Logs->Log("USERS", DEBUG,"New UUID for user: %s (%s)", uuid.c_str(), uid.empty() ? "allocated new" : "used remote");

	user_hash::iterator finduuid = ServerInstance->Users->uuidlist->find(uuid);
	if (finduuid == ServerInstance->Users->uuidlist->end())
		(*ServerInstance->Users->uuidlist)[uuid] = this;
	else
		throw CoreException("Duplicate UUID "+std::string(uuid)+" in User constructor");
}

User::~User()
{
	if (uuid.length())
		ServerInstance->Logs->Log("USERS", ERROR, "User destructor for %s called without cull", uuid.c_str());
}

const std::string& User::MakeHost()
{
	if (!this->cached_makehost.empty())
		return this->cached_makehost;

	char nhost[MAXBUF];
	/* This is much faster than snprintf */
	char* t = nhost;
	for(const char* n = ident.c_str(); *n; n++)
		*t++ = *n;
	*t++ = '@';
	for(const char* n = host.c_str(); *n; n++)
		*t++ = *n;
	*t = 0;

	this->cached_makehost.assign(nhost);

	return this->cached_makehost;
}

const std::string& User::MakeHostIP()
{
	if (!this->cached_hostip.empty())
		return this->cached_hostip;

	char ihost[MAXBUF];
	/* This is much faster than snprintf */
	char* t = ihost;
	for(const char* n = ident.c_str(); *n; n++)
		*t++ = *n;
	*t++ = '@';
	for(const char* n = this->GetIPString(); *n; n++)
		*t++ = *n;
	*t = 0;

	this->cached_hostip = ihost;

	return this->cached_hostip;
}

const std::string User::GetFullHost()
{
	if (!this->cached_fullhost.empty())
		return this->cached_fullhost;

	char result[MAXBUF];
	char* t = result;
	for(const char* n = nick.c_str(); *n; n++)
		*t++ = *n;
	*t++ = '!';
	for(const char* n = ident.c_str(); *n; n++)
		*t++ = *n;
	*t++ = '@';
	for(const char* n = dhost.c_str(); *n; n++)
		*t++ = *n;
	*t = 0;

	this->cached_fullhost = result;

	return this->cached_fullhost;
}

char* User::MakeWildHost()
{
	static char nresult[MAXBUF];
	char* t = nresult;
	*t++ = '*';	*t++ = '!';
	*t++ = '*';	*t++ = '@';
	for(const char* n = dhost.c_str(); *n; n++)
		*t++ = *n;
	*t = 0;
	return nresult;
}

const std::string User::GetFullRealHost()
{
	if (!this->cached_fullrealhost.empty())
		return this->cached_fullrealhost;

	char fresult[MAXBUF];
	char* t = fresult;
	for(const char* n = nick.c_str(); *n; n++)
		*t++ = *n;
	*t++ = '!';
	for(const char* n = ident.c_str(); *n; n++)
		*t++ = *n;
	*t++ = '@';
	for(const char* n = host.c_str(); *n; n++)
		*t++ = *n;
	*t = 0;

	this->cached_fullrealhost = fresult;

	return this->cached_fullrealhost;
}

bool User::IsInvited(const irc::string &channel)
{
	time_t now = ServerInstance->Time();
	InvitedList::iterator safei;
	for (InvitedList::iterator i = invites.begin(); i != invites.end(); ++i)
	{
		if (channel == i->first)
		{
			if (i->second != 0 && now > i->second)
			{
				/* Expired invite, remove it. */
				safei = i;
				--i;
				invites.erase(safei);
				continue;
			}
			return true;
		}
	}
	return false;
}

InvitedList* User::GetInviteList()
{
	time_t now = ServerInstance->Time();
	/* Weed out expired invites here. */
	InvitedList::iterator safei;
	for (InvitedList::iterator i = invites.begin(); i != invites.end(); ++i)
	{
		if (i->second != 0 && now > i->second)
		{
			/* Expired invite, remove it. */
			safei = i;
			--i;
			invites.erase(safei);
		}
	}
	return &invites;
}

void User::InviteTo(const irc::string &channel, time_t invtimeout)
{
	time_t now = ServerInstance->Time();
	if (invtimeout != 0 && now > invtimeout) return; /* Don't add invites that are expired from the get-go. */
	for (InvitedList::iterator i = invites.begin(); i != invites.end(); ++i)
	{
		if (channel == i->first)
		{
			if (i->second != 0 && invtimeout > i->second)
			{
				i->second = invtimeout;
			}

			return;
		}
	}
	invites.push_back(std::make_pair(channel, invtimeout));
}

void User::RemoveInvite(const irc::string &channel)
{
	for (InvitedList::iterator i = invites.begin(); i != invites.end(); i++)
	{
		if (channel == i->first)
		{
			invites.erase(i);
			return;
	 	}
	}
}

bool User::HasModePermission(unsigned char mode, ModeType type)
{
	if (!IS_LOCAL(this))
		return true;

	if (!IS_OPER(this))
		return false;

	if (mode < 'A' || mode > ('A' + 64)) return false;

	return ((type == MODETYPE_USER ? AllowedUserModes : AllowedChanModes))[(mode - 'A')];

}

bool User::HasPermission(const std::string &command)
{
	/*
	 * users on remote servers can completely bypass all permissions based checks.
	 * This prevents desyncs when one server has different type/class tags to another.
	 * That having been said, this does open things up to the possibility of source changes
	 * allowing remote kills, etc - but if they have access to the src, they most likely have
	 * access to the conf - so it's an end to a means either way.
	 */
	if (!IS_LOCAL(this))
		return true;

	// are they even an oper at all?
	if (!IS_OPER(this))
	{
		return false;
	}

	if (!AllowedOperCommands)
		return false;

	if (AllowedOperCommands->find(command) != AllowedOperCommands->end())
		return true;
	else if (AllowedOperCommands->find("*") != AllowedOperCommands->end())
		return true;

	return false;
}


bool User::HasPrivPermission(const std::string &privstr, bool noisy)
{
	if (!IS_LOCAL(this))
	{
		ServerInstance->Logs->Log("PRIVS", DEBUG, "Remote (yes)");
		return true;
	}

	if (!IS_OPER(this))
	{
		if (noisy)
			this->WriteServ("NOTICE %s :You are not an oper", this->nick.c_str());
		return false;
	}

	if (!AllowedPrivs)
	{
		if (noisy)
			this->WriteServ("NOTICE %s :Privset empty(!?)", this->nick.c_str());
		return false;
	}

	if (AllowedPrivs->find(privstr) != AllowedPrivs->end())
	{
		return true;
	}
	else if (AllowedPrivs->find("*") != AllowedPrivs->end())
	{
		return true;
	}

	if (noisy)
		this->WriteServ("NOTICE %s :Oper type %s does not have access to priv %s", this->nick.c_str(), this->oper.c_str(), privstr.c_str());
	return false;
}

void User::OnDataReady()
{
	if (quitting)
		return;

	if (MyClass && recvq.length() > MyClass->GetRecvqMax() && !HasPrivPermission("users/flood/increased-buffers"))
	{
		ServerInstance->Users->QuitUser(this, "RecvQ exceeded");
		ServerInstance->SNO->WriteToSnoMask('a', "User %s RecvQ of %lu exceeds connect class maximum of %lu",
			nick.c_str(), (unsigned long)recvq.length(), MyClass->GetRecvqMax());
	}
	unsigned long sendqmax = ULONG_MAX;
	if (MyClass && !HasPrivPermission("users/flood/increased-buffers"))
		sendqmax = MyClass->GetSendqSoftMax();

	while (Penalty < 10 && getSendQSize() < sendqmax)
	{
		std::string line;
		line.reserve(MAXBUF);
		std::string::size_type qpos = 0;
		while (qpos < recvq.length())
		{
			char c = recvq[qpos++];
			switch (c)
			{
			case '\0':
				c = ' ';
				break;
			case '\r':
				continue;
			case '\n':
				goto eol_found;
			}
			if (line.length() < MAXBUF - 2)
				line.push_back(c);
		}
		// if we got here, the recvq ran out before we found a newline
		return;
eol_found:
		// just found a newline. Terminate the string, and pull it out of recvq
		recvq = recvq.substr(qpos);

		// TODO should this be moved to when it was inserted in recvq?
		ServerInstance->stats->statsRecv += qpos;
		this->bytes_in += qpos;
		this->cmds_in++;

		ServerInstance->Parser->ProcessBuffer(line, this);
	}
	// Add pseudo-penalty so that we continue processing after sendq recedes
	if (Penalty == 0 && getSendQSize() >= sendqmax)
		Penalty++;
}

void User::AddWriteBuf(const std::string &data)
{
	// Don't bother sending text to remote users!
	if (IS_REMOTE(this))
		return;
	if (!quitting && MyClass && getSendQSize() + data.length() > MyClass->GetSendqHardMax() && !HasPrivPermission("users/flood/increased-buffers"))
	{
		/*
		 * Quit the user FIRST, because otherwise we could recurse
		 * here and hit the same limit.
		 */
		ServerInstance->Users->QuitUser(this, "SendQ exceeded");
		ServerInstance->SNO->WriteToSnoMask('a', "User %s SendQ exceeds connect class maximum of %lu",
			nick.c_str(), MyClass->GetSendqHardMax());
		return;
	}

	// We still want to append data to the sendq of a quitting user,
	// e.g. their ERROR message that says 'closing link'

	WriteData(data);
}

void User::OnError(BufferedSocketError)
{
	ServerInstance->Users->QuitUser(this, getError());
}

bool User::cull()
{
	if (!quitting)
		ServerInstance->Users->QuitUser(this, "Culled without QuitUser");
	if (uuid.empty())
	{
		ServerInstance->Logs->Log("USERS", DEBUG, "User culled twice? UUID empty");
		return true;
	}
	PurgeEmptyChannels();
	if (IS_LOCAL(this))
	{
		if (fd != INT_MAX)
			Close();

		std::vector<User*>::iterator x = find(ServerInstance->Users->local_users.begin(),ServerInstance->Users->local_users.end(),this);
		if (x != ServerInstance->Users->local_users.end())
			ServerInstance->Users->local_users.erase(x);
		else
			ServerInstance->Logs->Log("USERS", DEBUG, "Failed to remove user from vector");
	}

	if (this->AllowedOperCommands)
	{
		delete AllowedOperCommands;
		AllowedOperCommands = NULL;
	}

	if (this->AllowedPrivs)
	{
		delete AllowedPrivs;
		AllowedPrivs = NULL;
	}

	this->InvalidateCache();
	this->DecrementModes();

	if (client_sa.sa.sa_family != AF_UNSPEC)
		ServerInstance->Users->RemoveCloneCounts(this);

	ServerInstance->Users->uuidlist->erase(uuid);
	uuid.clear();
	return true;
}

void User::Oper(const std::string &opertype, const std::string &opername)
{
	if (this->IsModeSet('o'))
		this->UnOper();

	this->modes[UM_OPERATOR] = 1;
	this->WriteServ("MODE %s :+o", this->nick.c_str());
	FOREACH_MOD(I_OnOper, OnOper(this, opertype));

	ServerInstance->SNO->WriteToSnoMask('o',"%s (%s@%s) is now an IRC operator of type %s (using oper '%s')", this->nick.c_str(), this->ident.c_str(), this->host.c_str(), irc::Spacify(opertype.c_str()), opername.c_str());
	this->WriteNumeric(381, "%s :You are now %s %s", this->nick.c_str(), strchr("aeiouAEIOU", *opertype.c_str()) ? "an" : "a", irc::Spacify(opertype.c_str()));

	ServerInstance->Logs->Log("OPER", DEFAULT, "%s!%s@%s opered as type: %s", this->nick.c_str(), this->ident.c_str(), this->host.c_str(), opertype.c_str());
	this->oper.assign(opertype, 0, 512);
	ServerInstance->Users->all_opers.push_back(this);

	/*
	 * This might look like it's in the wrong place.
	 * It is *not*!
	 *
	 * For multi-network servers, we may not have the opertypes of the remote server, but we still want to mark the user as an oper of that type.
	 * -- w00t
	 */
	TagIndex::iterator iter_opertype = ServerInstance->Config->opertypes.find(this->oper.c_str());
	if (iter_opertype != ServerInstance->Config->opertypes.end())
	{
		if (AllowedOperCommands)
			AllowedOperCommands->clear();
		else
			AllowedOperCommands = new std::set<std::string>;

		if (AllowedPrivs)
			AllowedPrivs->clear();
		else
			AllowedPrivs = new std::set<std::string>;

		AllowedUserModes.reset();
		AllowedChanModes.reset();
		this->AllowedUserModes['o' - 'A'] = true; // Call me paranoid if you want.

		std::string myclass, mycmd, mypriv;
		irc::spacesepstream Classes(iter_opertype->second->getString("classes"));
		while (Classes.GetToken(myclass))
		{
			TagIndex::iterator iter_operclass = ServerInstance->Config->operclass.find(myclass.c_str());
			if (iter_operclass != ServerInstance->Config->operclass.end())
			{
				/* Process commands */
				irc::spacesepstream CommandList(iter_operclass->second->getString("commands"));
				while (CommandList.GetToken(mycmd))
				{
					this->AllowedOperCommands->insert(mycmd);
				}

				irc::spacesepstream PrivList(iter_operclass->second->getString("privs"));
				while (PrivList.GetToken(mypriv))
				{
					this->AllowedPrivs->insert(mypriv);
				}

				for (unsigned char* c = (unsigned char*)iter_operclass->second->getString("usermodes").c_str(); *c; ++c)
				{
					if (*c == '*')
					{
						this->AllowedUserModes.set();
					}
					else
					{
						this->AllowedUserModes[*c - 'A'] = true;
					}
				}

				for (unsigned char* c = (unsigned char*)iter_operclass->second->getString("chanmodes").c_str(); *c; ++c)
				{
					if (*c == '*')
					{
						this->AllowedChanModes.set();
					}
					else
					{
						this->AllowedChanModes[*c - 'A'] = true;
					}
				}
			}
		}
	}

	FOREACH_MOD(I_OnPostOper,OnPostOper(this, opertype, opername));
}

void User::UnOper()
{
	if (IS_OPER(this))
	{
		/*
		 * unset their oper type (what IS_OPER checks).
		 * note, order is important - this must come before modes as -o attempts
		 * to call UnOper. -- w00t
		 */
		this->oper.clear();


		/* Remove all oper only modes from the user when the deoper - Bug #466*/
		std::string moderemove("-");

		for (unsigned char letter = 'A'; letter <= 'z'; letter++)
		{
			ModeHandler* mh = ServerInstance->Modes->FindMode(letter, MODETYPE_USER);
			if (mh && mh->NeedsOper())
				moderemove += letter;
		}


		std::vector<std::string> parameters;
		parameters.push_back(this->nick);
		parameters.push_back(moderemove);

		ServerInstance->Parser->CallHandler("MODE", parameters, this);

		/* remove the user from the oper list. Will remove multiple entries as a safeguard against bug #404 */
		ServerInstance->Users->all_opers.remove(this);

		if (AllowedOperCommands)
		{
			delete AllowedOperCommands;
			AllowedOperCommands = NULL;
		}

		if (AllowedPrivs)
		{
			delete AllowedPrivs;
			AllowedPrivs = NULL;
		}

		AllowedUserModes.reset();
		AllowedChanModes.reset();
		this->modes[UM_OPERATOR] = 0;
	}
}

/* adds or updates an entry in the whowas list */
void User::AddToWhoWas()
{
	Module* whowas = ServerInstance->Modules->Find("cmd_whowas.so");
	if (whowas)
	{
		WhowasRequest req(NULL, whowas, WhowasRequest::WHOWAS_ADD);
		req.user = this;
		req.Send();
	}
}

/*
 * Check class restrictions
 */
void User::CheckClass()
{
	ConnectClass* a = this->MyClass;

	if ((!a) || (a->type == CC_DENY))
	{
		ServerInstance->Users->QuitUser(this, "Unauthorised connection");
		return;
	}
	else if ((a->GetMaxLocal()) && (ServerInstance->Users->LocalCloneCount(this) > a->GetMaxLocal()))
	{
		ServerInstance->Users->QuitUser(this, "No more connections allowed from your host via this connect class (local)");
		ServerInstance->SNO->WriteToSnoMask('a', "WARNING: maximum LOCAL connections (%ld) exceeded for IP %s", a->GetMaxLocal(), this->GetIPString());
		return;
	}
	else if ((a->GetMaxGlobal()) && (ServerInstance->Users->GlobalCloneCount(this) > a->GetMaxGlobal()))
	{
		ServerInstance->Users->QuitUser(this, "No more connections allowed from your host via this connect class (global)");
		ServerInstance->SNO->WriteToSnoMask('a', "WARNING: maximum GLOBAL connections (%ld) exceeded for IP %s", a->GetMaxGlobal(), this->GetIPString());
		return;
	}

	this->nping = ServerInstance->Time() + a->GetPingTime() + ServerInstance->Config->dns_timeout;
}

bool User::CheckLines(bool doZline)
{
	const char* check[] = { "G" , "K", (doZline) ? "Z" : NULL, NULL };

	if (!this->exempt)
	{
		for (int n = 0; check[n]; ++n)
		{
			XLine *r = ServerInstance->XLines->MatchesLine(check[n], this);

			if (r)
			{
				r->Apply(this);
				return true;
			}
		}
	}

	return false;
}

void User::FullConnect()
{
	ServerInstance->stats->statsConnects++;
	this->idle_lastmsg = ServerInstance->Time();

	/*
	 * You may be thinking "wtf, we checked this in User::AddClient!" - and yes, we did, BUT.
	 * At the time AddClient is called, we don't have a resolved host, by here we probably do - which
	 * may put the user into a totally seperate class with different restrictions! so we *must* check again.
	 * Don't remove this! -- w00t
	 */
	this->SetClass();

	/* Check the password, if one is required by the user's connect class.
	 * This CANNOT be in CheckClass(), because that is called prior to PASS as well!
	 */
	if (this->MyClass && !this->MyClass->GetPass().empty() && !this->haspassed)
	{
		ServerInstance->Users->QuitUser(this, "Invalid password");
		return;
	}

	if (this->CheckLines())
		return;

	this->WriteServ("NOTICE Auth :Welcome to \002%s\002!",ServerInstance->Config->Network.c_str());
	this->WriteNumeric(RPL_WELCOME, "%s :Welcome to the %s IRC Network %s!%s@%s",this->nick.c_str(), ServerInstance->Config->Network.c_str(), this->nick.c_str(), this->ident.c_str(), this->host.c_str());
	this->WriteNumeric(RPL_YOURHOSTIS, "%s :Your host is %s, running version InspIRCd-2.0",this->nick.c_str(),ServerInstance->Config->ServerName.c_str());
	this->WriteNumeric(RPL_SERVERCREATED, "%s :This server was created %s %s", this->nick.c_str(), __TIME__, __DATE__);
	this->WriteNumeric(RPL_SERVERVERSION, "%s %s InspIRCd-2.0 %s %s %s", this->nick.c_str(), ServerInstance->Config->ServerName.c_str(), ServerInstance->Modes->UserModeList().c_str(), ServerInstance->Modes->ChannelModeList().c_str(), ServerInstance->Modes->ParaModeList().c_str());

	ServerInstance->Config->Send005(this);
	this->WriteNumeric(RPL_YOURUUID, "%s %s :your unique ID", this->nick.c_str(), this->uuid.c_str());


	this->ShowMOTD();

	/* Now registered */
	if (ServerInstance->Users->unregistered_count)
		ServerInstance->Users->unregistered_count--;

	/* Trigger LUSERS output, give modules a chance too */
	ModResult MOD_RESULT;
	std::string command("LUSERS");
	std::vector<std::string> parameters;
	FIRST_MOD_RESULT(OnPreCommand, MOD_RESULT, (command, parameters, this, true, "LUSERS"));
	if (!MOD_RESULT)
		ServerInstance->CallCommandHandler(command, parameters, this);

	/*
	 * We don't set REG_ALL until triggering OnUserConnect, so some module events don't spew out stuff
	 * for a user that doesn't exist yet.
	 */
	FOREACH_MOD(I_OnUserConnect,OnUserConnect(this));

	this->registered = REG_ALL;

	FOREACH_MOD(I_OnPostConnect,OnPostConnect(this));

	ServerInstance->SNO->WriteToSnoMask('c',"Client connecting on port %d: %s!%s@%s [%s] [%s]",
		this->GetServerPort(), this->nick.c_str(), this->ident.c_str(), this->host.c_str(), this->GetIPString(), this->fullname.c_str());
	ServerInstance->Logs->Log("BANCACHE", DEBUG, "BanCache: Adding NEGATIVE hit for %s", this->GetIPString());
	ServerInstance->BanCache->AddHit(this->GetIPString(), "", "");
}

/** User::UpdateNick()
 * re-allocates a nick in the user_hash after they change nicknames,
 * returns a pointer to the new user as it may have moved
 */
User* User::UpdateNickHash(const char* New)
{
	//user_hash::iterator newnick;
	user_hash::iterator oldnick = ServerInstance->Users->clientlist->find(this->nick);

	if (!irc::string(this->nick.c_str()).compare(New))
		return oldnick->second;

	if (oldnick == ServerInstance->Users->clientlist->end())
		return NULL; /* doesnt exist */

	User* olduser = oldnick->second;
	ServerInstance->Users->clientlist->erase(oldnick);
	(*(ServerInstance->Users->clientlist))[New] = olduser;
	return olduser;
}

void User::InvalidateCache()
{
	/* Invalidate cache */
	cached_fullhost.clear();
	cached_hostip.clear();
	cached_makehost.clear();
	cached_fullrealhost.clear();
}

bool User::ForceNickChange(const char* newnick)
{
	ModResult MOD_RESULT;

	this->InvalidateCache();

	ServerInstance->NICKForced.set(this, 1);
	FIRST_MOD_RESULT(OnUserPreNick, MOD_RESULT, (this, newnick));
	ServerInstance->NICKForced.set(this, 0);

	if (MOD_RESULT == MOD_RES_DENY)
	{
		ServerInstance->stats->statsCollisions++;
		return false;
	}

	std::deque<classbase*> dummy;
	Command* nickhandler = ServerInstance->Parser->GetHandler("NICK");
	if (nickhandler) // wtfbbq, when would this not be here
	{
		std::vector<std::string> parameters;
		parameters.push_back(newnick);
		ServerInstance->NICKForced.set(this, 1);
		bool result = (ServerInstance->Parser->CallHandler("NICK", parameters, this) == CMD_SUCCESS);
		ServerInstance->NICKForced.set(this, 0);
		return result;
	}

	// Unreachable, we hope
	return false;
}

int User::GetServerPort()
{
	switch (this->server_sa.sa.sa_family)
	{
		case AF_INET6:
			return htons(this->server_sa.in6.sin6_port);
		case AF_INET:
			return htons(this->server_sa.in4.sin_port);
	}
	return 0;
}

const char* User::GetCIDRMask(int range)
{
	static char buf[44];

	if (range < 0)
		throw "Negative range, sorry, no.";

	/*
	 * Original code written by Oliver Lupton (Om).
	 * Integrated by me. Thanks. :) -- w00t
	 */
	switch (this->client_sa.sa.sa_family)
	{
		case AF_INET6:
		{
			/* unsigned char s6_addr[16]; */
			struct in6_addr v6;
			int i, bytestozero, extrabits;
			char buffer[40];

			if(range > 128)
				throw "CIDR mask width greater than address width (IPv6, 128 bit)";

			/* To create the CIDR mask we want to set all the bits after 'range' bits of the address
			 * to zero. This means the last (128 - range) bits of the address must be set to zero.
			 * Hence this number divided by 8 is the number of whole bytes from the end of the address
			 * which must be set to zero.
			 */
			bytestozero = (128 - range) / 8;

			/* Some of the least significant bits of the next most significant byte may also have to
			 * be zeroed. The number of bits is the remainder of the above division.
			 */
			extrabits = (128 - range) % 8;

			/* Populate our working struct with the parts of the user's IP which are required in the
			 * final CIDR mask. Set all the subsequent bytes to zero.
			 * (16 - bytestozero) is the number of bytes which must be populated with actual IP data.
			 */
			for(i = 0; i < (16 - bytestozero); i++)
			{
				v6.s6_addr[i] = client_sa.in6.sin6_addr.s6_addr[i];
			}

			/* And zero all the remaining bytes in the IP. */
			for(; i < 16; i++)
			{
				v6.s6_addr[i] = 0;
			}

			/* And finally, zero the extra bits required. */
			v6.s6_addr[15 - bytestozero] = (v6.s6_addr[15 - bytestozero] >> extrabits) << extrabits;

			snprintf(buf, 44, "%s/%d", inet_ntop(AF_INET6, &v6, buffer, 40), range);
			return buf;
		}
		break;
		case AF_INET:
		{
			struct in_addr v4;
			char buffer[16];

			if (range > 32)
				throw "CIDR mask width greater than address width (IPv4, 32 bit)";

			/* Users already have a sockaddr* pointer (User::ip) which contains either a v4 or v6 structure */
			v4.s_addr = client_sa.in4.sin_addr.s_addr;

			/* To create the CIDR mask we want to set all the bits after 'range' bits of the address
			 * to zero. This means the last (32 - range) bits of the address must be set to zero.
			 * This is done by shifting the value right and then back left by (32 - range) bits.
			 */
			if(range > 0)
			{
				v4.s_addr = ntohl(v4.s_addr);
				v4.s_addr = (v4.s_addr >> (32 - range)) << (32 - range);
				v4.s_addr = htonl(v4.s_addr);
			}
			else
			{
				/* a range of zero would cause a 32 bit value to be shifted by 32 bits.
				 * this has undefined behaviour, but for CIDR purposes the resulting mask
				 * from a.b.c.d/0 is 0.0.0.0/0
				 */
				v4.s_addr = 0;
			}

			snprintf(buf, 44, "%s/%d", inet_ntop(AF_INET, &v4, buffer, 16), range);
			return buf;
		}
		break;
	}

	return ""; // unused, but oh well
}

const char* User::GetIPString()
{
	int port;
	if (cachedip.empty())
	{
		irc::sockets::satoap(&client_sa, cachedip, port);
		/* IP addresses starting with a : on irc are a Bad Thing (tm) */
		if (cachedip.c_str()[0] == ':')
			cachedip.insert(0,1,'0');
	}

	return cachedip.c_str();
}

bool User::SetClientIP(const char* sip)
{
	this->cachedip = "";
	return irc::sockets::aptosa(sip, 0, &client_sa);
}

static std::string wide_newline("\r\n");

void User::Write(const std::string& text)
{
	if (!ServerInstance->SE->BoundsCheckFd(this))
		return;

	if (text.length() > MAXBUF - 2)
	{
		// this should happen rarely or never. Crop the string at 512 and try again.
		std::string try_again = text.substr(0, MAXBUF - 2);
		Write(try_again);
		return;
	}

	ServerInstance->Logs->Log("USEROUTPUT", DEBUG,"C[%d] O %s", this->GetFd(), text.c_str());

	this->AddWriteBuf(text);
	this->AddWriteBuf(wide_newline);

	ServerInstance->stats->statsSent += text.length() + 2;
	this->bytes_out += text.length() + 2;
	this->cmds_out++;
}

/** Write()
 */
void User::Write(const char *text, ...)
{
	va_list argsPtr;
	char textbuffer[MAXBUF];

	va_start(argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

	this->Write(std::string(textbuffer));
}

void User::WriteServ(const std::string& text)
{
	this->Write(":%s %s",ServerInstance->Config->ServerName.c_str(),text.c_str());
}

/** WriteServ()
 *  Same as Write(), except `text' is prefixed with `:server.name '.
 */
void User::WriteServ(const char* text, ...)
{
	va_list argsPtr;
	char textbuffer[MAXBUF];

	va_start(argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

	this->WriteServ(std::string(textbuffer));
}


void User::WriteNumeric(unsigned int numeric, const char* text, ...)
{
	va_list argsPtr;
	char textbuffer[MAXBUF];

	va_start(argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

	this->WriteNumeric(numeric, std::string(textbuffer));
}

void User::WriteNumeric(unsigned int numeric, const std::string &text)
{
	char textbuffer[MAXBUF];
	ModResult MOD_RESULT;

	FIRST_MOD_RESULT(OnNumeric, MOD_RESULT, (this, numeric, text));

	if (MOD_RESULT == MOD_RES_DENY)
		return;

	snprintf(textbuffer,MAXBUF,":%s %03u %s",ServerInstance->Config->ServerName.c_str(), numeric, text.c_str());
	this->Write(std::string(textbuffer));
}

void User::WriteFrom(User *user, const std::string &text)
{
	char tb[MAXBUF];

	snprintf(tb,MAXBUF,":%s %s",user->GetFullHost().c_str(),text.c_str());

	this->Write(std::string(tb));
}


/* write text from an originating user to originating user */

void User::WriteFrom(User *user, const char* text, ...)
{
	va_list argsPtr;
	char textbuffer[MAXBUF];

	va_start(argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

	this->WriteFrom(user, std::string(textbuffer));
}


/* write text to an destination user from a source user (e.g. user privmsg) */

void User::WriteTo(User *dest, const char *data, ...)
{
	char textbuffer[MAXBUF];
	va_list argsPtr;

	va_start(argsPtr, data);
	vsnprintf(textbuffer, MAXBUF, data, argsPtr);
	va_end(argsPtr);

	this->WriteTo(dest, std::string(textbuffer));
}

void User::WriteTo(User *dest, const std::string &data)
{
	dest->WriteFrom(this, data);
}

void User::WriteCommon(const char* text, ...)
{
	char textbuffer[MAXBUF];
	va_list argsPtr;

	if (this->registered != REG_ALL || quitting)
		return;

	int len = snprintf(textbuffer,MAXBUF,":%s ",this->GetFullHost().c_str());

	va_start(argsPtr, text);
	vsnprintf(textbuffer + len, MAXBUF - len, text, argsPtr);
	va_end(argsPtr);

	this->WriteCommonRaw(std::string(textbuffer), true);
}

void User::WriteCommonExcept(const char* text, ...)
{
	char textbuffer[MAXBUF];
	va_list argsPtr;

	if (this->registered != REG_ALL || quitting)
		return;

	int len = snprintf(textbuffer,MAXBUF,":%s ",this->GetFullHost().c_str());

	va_start(argsPtr, text);
	vsnprintf(textbuffer + len, MAXBUF - len, text, argsPtr);
	va_end(argsPtr);

	this->WriteCommonRaw(std::string(textbuffer), false);
}

void User::WriteCommonRaw(const std::string &line, bool include_self)
{
	if (this->registered != REG_ALL || quitting)
		return;

	uniq_id_t uniq_id = ++already_sent;

	UserChanList include_c(chans);
	std::map<User*,bool> exceptions;

	exceptions[this] = include_self;

	FOREACH_MOD(I_OnBuildNeighborList,OnBuildNeighborList(this, include_c, exceptions));

	for (std::map<User*,bool>::iterator i = exceptions.begin(); i != exceptions.end(); ++i)
	{
		User* u = i->first;
		if (IS_LOCAL(u) && !u->quitting)
		{
			already_sent[u->fd] = uniq_id;
			if (i->second)
				u->Write(line);
		}
	}
	for (UCListIter v = include_c.begin(); v != include_c.end(); ++v)
	{
		Channel* c = *v;
		const UserMembList* ulist = c->GetUsers();
		for (UserMembList::const_iterator i = ulist->begin(); i != ulist->end(); i++)
		{
			User* u = i->first;
			if (IS_LOCAL(u) && !u->quitting && already_sent[u->fd] != uniq_id)
			{
				already_sent[u->fd] = uniq_id;
				u->Write(line);
			}
		}
	}
}

void User::WriteCommonQuit(const std::string &normal_text, const std::string &oper_text)
{
	char tb1[MAXBUF];
	char tb2[MAXBUF];

	if (this->registered != REG_ALL)
		return;

	uniq_id_t uniq_id = ++already_sent;

	snprintf(tb1,MAXBUF,":%s QUIT :%s",this->GetFullHost().c_str(),normal_text.c_str());
	snprintf(tb2,MAXBUF,":%s QUIT :%s",this->GetFullHost().c_str(),oper_text.c_str());
	std::string out1 = tb1;
	std::string out2 = tb2;

	UserChanList include_c(chans);
	std::map<User*,bool> exceptions;

	FOREACH_MOD(I_OnBuildNeighborList,OnBuildNeighborList(this, include_c, exceptions));

	for (std::map<User*,bool>::iterator i = exceptions.begin(); i != exceptions.end(); ++i)
	{
		User* u = i->first;
		if (IS_LOCAL(u) && !u->quitting)
		{
			already_sent[u->fd] = uniq_id;
			if (i->second)
				u->Write(IS_OPER(u) ? out2 : out1);
		}
	}
	for (UCListIter v = include_c.begin(); v != include_c.end(); ++v)
	{
		const UserMembList* ulist = (*v)->GetUsers();
		for (UserMembList::const_iterator i = ulist->begin(); i != ulist->end(); i++)
		{
			User* u = i->first;
			if (IS_LOCAL(u) && !u->quitting && (already_sent[u->fd] != uniq_id))
			{
				already_sent[u->fd] = uniq_id;
				u->Write(IS_OPER(u) ? out2 : out1);
			}
		}
	}
}

void User::WriteWallOps(const std::string &text)
{
	std::string wallop("WALLOPS :");
	wallop.append(text);

	for (std::vector<User*>::const_iterator i = ServerInstance->Users->local_users.begin(); i != ServerInstance->Users->local_users.end(); i++)
	{
		User* t = *i;
		if (t->IsModeSet('w'))
			this->WriteTo(t,wallop);
	}
}

void User::WriteWallOps(const char* text, ...)
{
	if (!IS_LOCAL(this))
		return;

	char textbuffer[MAXBUF];
	va_list argsPtr;

	va_start(argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

	this->WriteWallOps(std::string(textbuffer));
}

/* return 0 or 1 depending if users u and u2 share one or more common channels
 * (used by QUIT, NICK etc which arent channel specific notices)
 *
 * The old algorithm in 1.0 for this was relatively inefficient, iterating over
 * the first users channels then the second users channels within the outer loop,
 * therefore it was a maximum of x*y iterations (upon returning 0 and checking
 * all possible iterations). However this new function instead checks against the
 * channel's userlist in the inner loop which is a std::map<User*,User*>
 * and saves us time as we already know what pointer value we are after.
 * Don't quote me on the maths as i am not a mathematician or computer scientist,
 * but i believe this algorithm is now x+(log y) maximum iterations instead.
 */
bool User::SharesChannelWith(User *other)
{
	if ((!other) || (this->registered != REG_ALL) || (other->registered != REG_ALL))
		return false;

	/* Outer loop */
	for (UCListIter i = this->chans.begin(); i != this->chans.end(); i++)
	{
		/* Eliminate the inner loop (which used to be ~equal in size to the outer loop)
		 * by replacing it with a map::find which *should* be more efficient
		 */
		if ((*i)->HasUser(other))
			return true;
	}
	return false;
}

bool User::ChangeName(const char* gecos)
{
	if (!this->fullname.compare(gecos))
		return true;

	if (IS_LOCAL(this))
	{
		ModResult MOD_RESULT;
		FIRST_MOD_RESULT(OnChangeLocalUserGECOS, MOD_RESULT, (this,gecos));
		if (MOD_RESULT == MOD_RES_DENY)
			return false;
		FOREACH_MOD(I_OnChangeName,OnChangeName(this,gecos));
	}
	this->fullname.assign(gecos, 0, ServerInstance->Config->Limits.MaxGecos);

	return true;
}

void User::DoHostCycle(const std::string &quitline)
{
	char buffer[MAXBUF];

	if (!ServerInstance->Config->CycleHosts)
		return;

	uniq_id_t silent_id = ++already_sent;
	uniq_id_t seen_id = ++already_sent;

	UserChanList include_c(chans);
	std::map<User*,bool> exceptions;

	FOREACH_MOD(I_OnBuildNeighborList,OnBuildNeighborList(this, include_c, exceptions));

	for (std::map<User*,bool>::iterator i = exceptions.begin(); i != exceptions.end(); ++i)
	{
		User* u = i->first;
		if (IS_LOCAL(u) && !u->quitting)
		{
			if (i->second)
			{
				already_sent[u->fd] = seen_id;
				u->Write(quitline);
			}
			else
			{
				already_sent[u->fd] = silent_id;
			}
		}
	}
	for (UCListIter v = include_c.begin(); v != include_c.end(); ++v)
	{
		Channel* c = *v;
		snprintf(buffer, MAXBUF, ":%s JOIN %s", GetFullHost().c_str(), c->name.c_str());
		std::string joinline(buffer);
		std::string modeline = ServerInstance->Modes->ModeString(this, c);
		if (modeline.length() > 0)
		{
			snprintf(buffer, MAXBUF, ":%s MODE %s +%s", GetFullHost().c_str(), c->name.c_str(), modeline.c_str());
			modeline = buffer;
		}

		const UserMembList *ulist = c->GetUsers();
		for (UserMembList::const_iterator i = ulist->begin(); i != ulist->end(); i++)
		{
			User* u = i->first;
			if (u == this || !IS_LOCAL(u))
				continue;
			if (already_sent[u->fd] == silent_id)
				continue;

			if (already_sent[u->fd] != seen_id)
			{
				u->Write(quitline);
				already_sent[i->first->fd] = seen_id;
			}
			u->Write(joinline);
			if (modeline.length() > 0)
				u->Write(modeline);
		}
	}
}

bool User::ChangeDisplayedHost(const char* shost)
{
	if (dhost == shost)
		return true;

	if (IS_LOCAL(this))
	{
		ModResult MOD_RESULT;
		FIRST_MOD_RESULT(OnChangeLocalUserHost, MOD_RESULT, (this,shost));
		if (MOD_RESULT == MOD_RES_DENY)
			return false;
	}

	FOREACH_MOD(I_OnChangeHost, OnChangeHost(this,shost));

	std::string quitstr = ":" + GetFullHost() + " QUIT :Changing host";

	/* Fix by Om: User::dhost is 65 long, this was truncating some long hosts */
	this->dhost.assign(shost, 0, 64);

	this->InvalidateCache();

	this->DoHostCycle(quitstr);

	if (IS_LOCAL(this))
		this->WriteNumeric(RPL_YOURDISPLAYEDHOST, "%s %s :is now your displayed host",this->nick.c_str(),this->dhost.c_str());

	return true;
}

bool User::ChangeIdent(const char* newident)
{
	if (this->ident == newident)
		return true;

	FOREACH_MOD(I_OnChangeIdent, OnChangeIdent(this,newident));

	std::string quitstr = ":" + GetFullHost() + " QUIT :Changing ident";

	this->ident.assign(newident, 0, ServerInstance->Config->Limits.IdentMax + 1);

	this->InvalidateCache();

	this->DoHostCycle(quitstr);

	return true;
}

void User::SendAll(const char* command, const char* text, ...)
{
	char textbuffer[MAXBUF];
	char formatbuffer[MAXBUF];
	va_list argsPtr;

	va_start(argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

	snprintf(formatbuffer,MAXBUF,":%s %s $* :%s", this->GetFullHost().c_str(), command, textbuffer);
	std::string fmt = formatbuffer;

	for (std::vector<User*>::const_iterator i = ServerInstance->Users->local_users.begin(); i != ServerInstance->Users->local_users.end(); i++)
	{
		(*i)->Write(fmt);
	}
}


std::string User::ChannelList(User* source, bool spy)
{
	std::string list;

	for (UCListIter i = this->chans.begin(); i != this->chans.end(); i++)
	{
		Channel* c = *i;
		/* If the target is the sender, neither +p nor +s is set, or
		 * the channel contains the user, it is not a spy channel
		 */
		if (spy != (source == this || !(c->IsModeSet('p') || c->IsModeSet('s')) || c->HasUser(source)))
			list.append(c->GetPrefixChar(this)).append(c->name).append(" ");
	}

	return list;
}

void User::SplitChanList(User* dest, const std::string &cl)
{
	std::string line;
	std::ostringstream prefix;
	std::string::size_type start, pos, length;

	prefix << this->nick << " " << dest->nick << " :";
	line = prefix.str();
	int namelen = ServerInstance->Config->ServerName.length() + 6;

	for (start = 0; (pos = cl.find(' ', start)) != std::string::npos; start = pos+1)
	{
		length = (pos == std::string::npos) ? cl.length() : pos;

		if (line.length() + namelen + length - start > 510)
		{
			ServerInstance->SendWhoisLine(this, dest, 319, "%s", line.c_str());
			line = prefix.str();
		}

		if(pos == std::string::npos)
		{
			line.append(cl.substr(start, length - start));
			break;
		}
		else
		{
			line.append(cl.substr(start, length - start + 1));
		}
	}

	if (line.length())
	{
		ServerInstance->SendWhoisLine(this, dest, 319, "%s", line.c_str());
	}
}

/*
 * Sets a user's connection class.
 * If the class name is provided, it will be used. Otherwise, the class will be guessed using host/ip/ident/etc.
 * NOTE: If the <ALLOW> or <DENY> tag specifies an ip, and this user resolves,
 * then their ip will be taken as 'priority' anyway, so for example,
 * <connect allow="127.0.0.1"> will match joe!bloggs@localhost
 */
ConnectClass* User::SetClass(const std::string &explicit_name)
{
	ConnectClass *found = NULL;

	if (!IS_LOCAL(this))
		return NULL;

	ServerInstance->Logs->Log("CONNECTCLASS", DEBUG, "Setting connect class for UID %s", this->uuid.c_str());

	if (!explicit_name.empty())
	{
		for (ClassVector::iterator i = ServerInstance->Config->Classes.begin(); i != ServerInstance->Config->Classes.end(); i++)
		{
			ConnectClass* c = *i;

			if (explicit_name == c->name)
			{
				ServerInstance->Logs->Log("CONNECTCLASS", DEBUG, "Explicitly set to %s", explicit_name.c_str());
				found = c;
			}
		}
	}
	else
	{
		for (ClassVector::iterator i = ServerInstance->Config->Classes.begin(); i != ServerInstance->Config->Classes.end(); i++)
		{
			ConnectClass* c = *i;

			if (c->type == CC_ALLOW)
			{
				ServerInstance->Logs->Log("CONNECTCLASS", DEBUG, "ALLOW %s %d %s", c->host.c_str(), c->GetPort(), c->GetName().c_str());
			}
			else
			{
				ServerInstance->Logs->Log("CONNECTCLASS", DEBUG, "DENY %s %d %s", c->GetHost().c_str(), c->GetPort(), c->GetName().c_str());
			}

			/* check if host matches.. */
			if (c->GetHost().length() && !InspIRCd::MatchCIDR(this->GetIPString(), c->GetHost(), NULL) &&
			    !InspIRCd::MatchCIDR(this->host, c->GetHost(), NULL))
			{
				ServerInstance->Logs->Log("CONNECTCLASS", DEBUG, "No host match (for %s)", c->GetHost().c_str());
				continue;
			}

			/*
			 * deny change if change will take class over the limit check it HERE, not after we found a matching class,
			 * because we should attempt to find another class if this one doesn't match us. -- w00t
			 */
			if (c->limit && (c->GetReferenceCount() >= c->limit))
			{
				ServerInstance->Logs->Log("CONNECTCLASS", DEBUG, "OOPS: Connect class limit (%lu) hit, denying", c->limit);
				continue;
			}

			/* if it requires a port ... */
			if (c->GetPort())
			{
				ServerInstance->Logs->Log("CONNECTCLASS", DEBUG, "Requires port (%d)", c->GetPort());

				/* and our port doesn't match, fail. */
				if (this->GetServerPort() != c->GetPort())
				{
					ServerInstance->Logs->Log("CONNECTCLASS", DEBUG, "Port match failed (%d)", this->GetServerPort());
					continue;
				}
			}

			/* we stop at the first class that meets ALL critera. */
			found = c;
			break;
		}
	}

	/*
	 * Okay, assuming we found a class that matches.. switch us into that class, keeping refcounts up to date.
	 */
	if (found)
	{
		MyClass = found;
	}

	return this->MyClass;
}

/* looks up a users password for their connection class (<ALLOW>/<DENY> tags)
 * NOTE: If the <ALLOW> or <DENY> tag specifies an ip, and this user resolves,
 * then their ip will be taken as 'priority' anyway, so for example,
 * <connect allow="127.0.0.1"> will match joe!bloggs@localhost
 */
ConnectClass* User::GetClass()
{
	return this->MyClass;
}

void User::PurgeEmptyChannels()
{
	std::vector<Channel*> to_delete;

	// firstly decrement the count on each channel
	for (UCListIter f = this->chans.begin(); f != this->chans.end(); f++)
	{
		Channel* c = *f;
		c->RemoveAllPrefixes(this);
		if (c->DelUser(this) == 0)
		{
			/* No users left in here, mark it for deletion */
			try
			{
				to_delete.push_back(c);
			}
			catch (...)
			{
				ServerInstance->Logs->Log("USERS", DEBUG,"Exception in User::PurgeEmptyChannels to_delete.push_back()");
			}
		}
	}

	for (std::vector<Channel*>::iterator n = to_delete.begin(); n != to_delete.end(); n++)
	{
		Channel* thischan = *n;
		chan_hash::iterator i2 = ServerInstance->chanlist->find(thischan->name);
		if (i2 != ServerInstance->chanlist->end())
		{
			ModResult MOD_RESULT;
			FIRST_MOD_RESULT(OnChannelPreDelete, MOD_RESULT, (i2->second));
			if (MOD_RESULT == MOD_RES_DENY)
				continue; // delete halted by module
			FOREACH_MOD(I_OnChannelDelete,OnChannelDelete(i2->second));
			delete i2->second;
			ServerInstance->chanlist->erase(i2);
			this->chans.erase(*n);
		}
	}

	this->UnOper();
}

void User::ShowMOTD()
{
	if (!ServerInstance->Config->MOTD.size())
	{
		this->WriteNumeric(ERR_NOMOTD, "%s :Message of the day file is missing.",this->nick.c_str());
		return;
	}
	this->WriteNumeric(RPL_MOTDSTART, "%s :%s message of the day", this->nick.c_str(), ServerInstance->Config->ServerName.c_str());

	for (file_cache::iterator i = ServerInstance->Config->MOTD.begin(); i != ServerInstance->Config->MOTD.end(); i++)
		this->WriteNumeric(RPL_MOTD, "%s :- %s",this->nick.c_str(),i->c_str());

	this->WriteNumeric(RPL_ENDOFMOTD, "%s :End of message of the day.", this->nick.c_str());
}

void User::ShowRULES()
{
	if (!ServerInstance->Config->RULES.size())
	{
		this->WriteNumeric(ERR_NORULES, "%s :RULES File is missing",this->nick.c_str());
		return;
	}

	this->WriteNumeric(RPL_RULESTART, "%s :- %s Server Rules -",this->nick.c_str(),ServerInstance->Config->ServerName.c_str());

	for (file_cache::iterator i = ServerInstance->Config->RULES.begin(); i != ServerInstance->Config->RULES.end(); i++)
		this->WriteNumeric(RPL_RULES, "%s :- %s",this->nick.c_str(),i->c_str());

	this->WriteNumeric(RPL_RULESEND, "%s :End of RULES command.",this->nick.c_str());
}

void User::IncreasePenalty(int increase)
{
	this->Penalty += increase;
}

void User::DecreasePenalty(int decrease)
{
	this->Penalty -= decrease;
}

void FakeUser::SetFakeServer(std::string name)
{
	this->nick = name;
	this->server = name;
}

const std::string FakeUser::GetFullHost()
{
	if (!ServerInstance->Config->HideWhoisServer.empty())
		return ServerInstance->Config->HideWhoisServer;
	return nick;
}

const std::string FakeUser::GetFullRealHost()
{
	if (!ServerInstance->Config->HideWhoisServer.empty())
		return ServerInstance->Config->HideWhoisServer;
	return nick;
}

ConnectClass::ConnectClass(char t, const std::string& mask)
	: type(t), name("unnamed"), registration_timeout(0), host(mask),
	pingtime(0), pass(""), hash(""), softsendqmax(0), hardsendqmax(0),
	recvqmax(0), maxlocal(0), maxglobal(0), maxchans(0), port(0), limit(0)
{
}

ConnectClass::ConnectClass(char t, const std::string& mask, const ConnectClass& parent)
	: type(t), name("unnamed"),
	registration_timeout(parent.registration_timeout), host(mask),
	pingtime(parent.pingtime), pass(parent.pass), hash(parent.hash),
	softsendqmax(parent.softsendqmax), hardsendqmax(parent.hardsendqmax),
	recvqmax(parent.recvqmax), maxlocal(parent.maxlocal),
	maxglobal(parent.maxglobal), maxchans(parent.maxchans),
	port(parent.port), limit(parent.limit)
{
}

void ConnectClass::Update(const ConnectClass* src)
{
	name = src->name;
	registration_timeout = src->registration_timeout;
	host = src->host;
	pingtime = src->pingtime;
	pass = src->pass;
	hash = src->hash;
	softsendqmax = src->softsendqmax;
	hardsendqmax = src->hardsendqmax;
	recvqmax = src->recvqmax;
	maxlocal = src->maxlocal;
	maxglobal = src->maxglobal;
	limit = src->limit;
}
