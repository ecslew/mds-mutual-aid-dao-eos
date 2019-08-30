#include "medishares.hpp"
#include <eosiolib/time.hpp>

using namespace eosio;

asset medishares::keymarket::convert_to_exchange( connector& c, asset in ) {
    real_type R(supply.amount);
    real_type C(c.balance.amount+in.amount);
    real_type F(c.weight/1000.0);
    real_type T(in.amount);
    real_type ONE(1.0);

    real_type E = -R * (ONE - std::pow( ONE + T / C, F) );
    int64_t issued = int64_t(E);

    supply.amount += issued;
    c.balance.amount += in.amount;

    base.balance.amount -= issued;

    return asset( issued, supply.symbol );
}

asset medishares::keymarket::convert_from_exchange( connector& c, asset in ) {
    eosio_assert( in.symbol== supply.symbol, "unexpected asset symbol input" );

    real_type R(supply.amount - in.amount);
    real_type C(c.balance.amount);
    real_type F(1000.0/c.weight);
    real_type E(in.amount);
    real_type ONE(1.0);

    real_type T = C * (std::pow( ONE + E/R, F) - ONE);
    int64_t out = int64_t(T);

    supply.amount -= in.amount;
    c.balance.amount -= out;

    base.balance.amount += in.amount;

    return asset( out, c.balance.symbol );
}

asset medishares::keymarket::convert( asset from, symbol_type to ) {
    auto sell_symbol  = from.symbol;
    auto ex_symbol    = supply.symbol;
    auto base_symbol  = base.balance.symbol;
    auto quote_symbol = quote.balance.symbol;

    if( sell_symbol != ex_symbol ) {
        if( sell_symbol == base_symbol ) {
            from = convert_to_exchange( base, from );
        } else if( sell_symbol == quote_symbol ) {
            from = convert_to_exchange( quote, from );
        } else {
            eosio_assert( false, "invalid sell" );
        }
    } else {
        if( to == base_symbol ) {
            from = convert_from_exchange( base, from );
        } else if( to == quote_symbol ) {
            from = convert_from_exchange( quote, from );
        } else {
            eosio_assert( false, "invalid conversion" );
        }
    }

    if( to != from.symbol )
        return convert( from, to );

    return from;
}

void medishares::init(const uint64_t guarantee_rate, const uint64_t ref_rate)
{
    eosio_assert(ref_rate > 0 && guarantee_rate > 0, "must positive rate");
    eosio_assert((ref_rate + guarantee_rate) < 1000, "invalid parameters");

    require_auth(_self);

    auto itr = keymarket.find(KEY_SYMBOL);
    eosio_assert(itr == keymarket.end(), "key market already created");

    itr = keymarket.emplace(_self, [&](auto& k) {
        k.supply.amount = 1000000;
        k.supply.symbol = KEY_SYMBOL;
        k.base.balance.amount = 1000000;
        k.base.balance.symbol = KEY_SYMBOL;
        k.quote.balance.amount = 100 * 10000;
        k.quote.balance.symbol = CORE_SYMBOL;
    });

    auto glb = global.begin();
    eosio_assert(glb == global.end(), "global table already created");
    glb = global.emplace(_self, [&](auto& gl){
        gl.ref_rate = ref_rate;
        gl.guarantee_rate = guarantee_rate;
        gl.guarantee_pool = asset(0, CORE_SYMBOL);
        gl.bonus_pool = asset(0, CORE_SYMBOL);
        gl.cases_num = 0;
    }); 
}

void medishares::handleTransfer(const account_name from, const account_name to, const asset& quantity, string memo)
{
    eosio_assert(quantity.symbol == CORE_SYMBOL, "unsupported symbol");
    eosio_assert(quantity.amount >= 1000, "must greater than 0.1 EOS");

    require_auth(from);

    account_name participator = from;
    account_name referrer = 0;
    uint64_t ref_amount = 0;

    memo.erase(memo.begin(), find_if(memo.begin(), memo.end(), [](int ch) {
        return !isspace(ch);
    }));
    memo.erase(find_if(memo.rbegin(), memo.rend(), [](int ch) {
        return !isspace(ch);
    }).base(), memo.end());

    //memo:"buyfor":"xxxxxxxxxxxx","ref":"xxxxxxxxxxxx"
    auto separator_pos = memo.find("\"buyfor\":\"");
    if (separator_pos != string::npos) {
        auto end_pos =  memo.find("\"", separator_pos+10); 
        eosio_assert(end_pos != string::npos, "parse memo error");
        eosio_assert(end_pos - separator_pos <= 22 && end_pos - separator_pos > 10, "invalid account name"); 
        string beneficiary_account_str = memo.substr(separator_pos + 10, end_pos - 1);
        participator = string_to_name(beneficiary_account_str.c_str()); 
        eosio_assert(is_account(participator), "participator account does not exist");
    }

    separator_pos = memo.find("\"ref\":\"");
    if (separator_pos != string::npos) {
        auto end_pos =  memo.find("\"", separator_pos+7); 
        eosio_assert(end_pos != string::npos, "parse memo error");
        eosio_assert(end_pos - separator_pos <= 19 && end_pos - separator_pos > 7, "invalid account name");
        string referral_account_str = memo.substr(separator_pos + 7, end_pos - 1);
        referrer = string_to_name(referral_account_str.c_str());
        eosio_assert(is_account(referrer), "referrer account does not exist");
    }

    auto glb = global.begin();
    eosio_assert(glb != global.end(), "the global table does not exist");
    
    if(referrer != 0){
        ref_amount = (uint64_t)(quantity.amount * glb->ref_rate / 1000);
        eosio_assert(ref_amount > 0, "referral asset too small");

        action(
            permission_level{_self, N(active)},
            N(eosio.token), N(transfer),
            std::make_tuple(_self, referrer, asset(ref_amount, CORE_SYMBOL), std::string("Referral bonuses"))
        ).send();
    }

    uint64_t guarantee_amount = (uint64_t)(quantity.amount * glb->guarantee_rate / 1000); 
    uint64_t bonus_amount = quantity.amount - ref_amount - guarantee_amount;
    eosio_assert(bonus_amount > 0, "bonus amount abnormity");
    global.modify(glb, 0, [&](auto& gl){
        gl.guarantee_pool = gl.guarantee_pool + asset(guarantee_amount, CORE_SYMBOL);
        gl.bonus_pool = gl.bonus_pool + asset(bonus_amount, CORE_SYMBOL);
    });

    accounts account_t( _self, participator );
    auto itr = account_t.find(CORE_SYMBOL);
    if( itr ==  account_t.end() ) {
        account_t.emplace( _self, [&]( auto& a ){
            a.balance = asset(guarantee_amount, CORE_SYMBOL);
        });
    } else {
        account_t.modify( itr, 0, [&]( auto& a ) {
            a.balance += asset(guarantee_amount, CORE_SYMBOL);
        });
    }

    auto key_out = asset(0, KEY_SYMBOL);
    const auto& market = keymarket.get(KEY_SYMBOL, "key market does not exist");
    keymarket.modify( market, 0, [&]( auto& km ) {
        key_out = km.convert( asset(bonus_amount, CORE_SYMBOL), KEY_SYMBOL);
    });

    eosio_assert( key_out.amount > 0, "must reserve a positive amount" );

    itr = account_t.find( key_out.symbol.name() );
    if( itr ==  account_t.end() ) {
        account_t.emplace( _self, [&]( auto& a ){
            a.balance = key_out;
        });
    } else {
        account_t.modify( itr, 0, [&]( auto& a ) {
            a.balance += key_out;
        });
    }
}

asset medishares::get_balance(account_name owner, symbol_name sym)const
{
    accounts accountstable(_self, owner);
    const auto& ac = accountstable.get(sym);
    return ac.balance;
}

void medishares::sellkey(account_name account, asset key_quantity){
    require_auth(account);
    eosio_assert(key_quantity.amount > 0, "quantity cannot be negative");
    eosio_assert(key_quantity.symbol == KEY_SYMBOL, "this asset does not supported");
    const auto& market = keymarket.get(key_quantity.symbol, "this asset market does not exist");
    accounts account_t(_self, account);
    auto itr = account_t.find(key_quantity.symbol.name());
    eosio_assert(itr != account_t.end(),"the user do not have the asset");
    eosio_assert(itr->balance.amount >= key_quantity.amount, "insufficient quota");

    asset tokens_out;
    keymarket.modify(market, 0, [&](auto& km){
        tokens_out = km.convert(key_quantity, CORE_SYMBOL);
    });
    eosio_assert(tokens_out.amount > 0, "token amount too small to transfer");
    action(
        permission_level{_self, N(active)},
        N(eosio.token), N(transfer),
        std::make_tuple(_self, account, tokens_out, std::string("sell "+std::to_string(key_quantity.amount)+" key"))
    ).send();

    if(itr->balance.amount == key_quantity.amount){
        account_t.erase(itr);
    }else{
        account_t.modify(itr, account, [&](auto& a){
          a.balance -= key_quantity;
        });
    }
}

void medishares::transfer(account_name from, account_name to, asset quantity, string memo)
{
    eosio_assert(from != to, "cannot transfer to self");
    require_auth(from);
    eosio_assert(is_account(to), "to account does not exist");

    require_recipient(from);
    require_recipient(to);

    eosio_assert(quantity.is_valid(), "invalid quantity");
    eosio_assert(quantity.amount > 0, "must transfer positive quantity");
    eosio_assert(quantity.symbol == KEY_SYMBOL, "this asset is not supported or the symbol precision mismatch");
    eosio_assert(memo.size() <= 256, "memo has more than 256 bytes");

    sub_balance(from, quantity);
    add_balance(to, quantity, from);
}

void medishares::sub_balance(account_name owner, asset value){
    accounts from_acnts(_self, owner);

    const auto& from = from_acnts.get(value.symbol.name(), "no balance object found");
    eosio_assert(from.balance.amount >= value.amount, "overdrawn balance");

    if(from.balance.amount == value.amount) {
        from_acnts.erase(from);
    }else{
        from_acnts.modify(from, owner, [&](auto& a){
            a.balance -= value;
        });
    }
}

void medishares::add_balance(account_name owner, asset value, account_name ram_payer)
{
    accounts to_acnts(_self, owner);
    auto to = to_acnts.find(value.symbol.name());
    if(to == to_acnts.end()){
        to_acnts.emplace(ram_payer, [&](auto& a){
            a.balance = value;
        });
    }else{
        to_acnts.modify(to, 0, [&](auto& a){
            a.balance += value;
        });
    }
}

void medishares::stakekey(account_name account, asset key_quantity){
    require_auth(account);
    eosio_assert(key_quantity.amount > 0, "quantity cannot be negative");
    eosio_assert(key_quantity.symbol == KEY_SYMBOL, "this asset is not supported or the symbol precision mismatch");

    sub_balance(account, key_quantity);
    add_balance(account, asset(key_quantity.amount, STAKE_SYMBOL), account);

    auto voters_itr = voters.find(account);

    //若未投票，直接结束
    if(voters_itr == voters.end())
        return;

    //遍历所投过的case，更新case票数，若case已过投票窗口期，则不更新；若case已删除，则删除对应投票项
    for(auto list_itr = voters_itr->vote_list.begin(); list_itr != voters_itr->vote_list.end(); list_itr ++){
        auto case_itr = cases.find(list_itr->case_id);
        if(case_itr == cases.end()){
            voters.modify(voters_itr, account, [&]( auto& v){
                v.vote_list.erase(list_itr);
            });        
        }else{
            if(case_itr->start_time + TIME_WINDOW_FOR_VOTE < now()){
                continue;
            }
            if(list_itr->agreed){
                cases.modify(case_itr, account, [&]( auto& c){
                    c.vote_yes += asset(key_quantity.amount, STAKE_SYMBOL);
                });
            }else{
                cases.modify(case_itr, account, [&]( auto& c){
                    c.vote_no += asset(key_quantity.amount, STAKE_SYMBOL);
                });
            }
        }
    }
}

void medishares::unstakekey(account_name account, asset key_quantity){
    require_auth(account);
    eosio_assert(key_quantity.amount > 0, "quantity cannot be negative");
    eosio_assert(key_quantity.symbol == STAKE_SYMBOL, "this asset is not supported or the symbol precision mismatch");

    sub_balance(account, key_quantity);
    add_balance(account, asset(key_quantity.amount, KEY_SYMBOL), account);

    accounts account_t(_self, account);
    auto st_itr = account_t.find(asset(0,STAKE_SYMBOL).symbol.name());
    auto voters_itr = voters.find(account);

    //若未投票，直接结束
    if(voters_itr == voters.end())
        return;

    //遍历所投过的case，更新case票数，若case已过投票窗口期，则不更新；若case已删除，则删除对应投票项；若对所有STKEY unstake，则删除投票列表
    for(auto list_itr = voters_itr->vote_list.begin(); list_itr != voters_itr->vote_list.end(); list_itr ++){
        auto case_itr = cases.find(list_itr->case_id);
        if(case_itr == cases.end()){
            voters.modify(voters_itr, account, [&]( auto& v){
                v.vote_list.erase(list_itr);
            });
        }else{
            if(case_itr->start_time + TIME_WINDOW_FOR_VOTE >= now()){
                if(list_itr->agreed){
                    cases.modify(case_itr, account, [&]( auto& c){
                        c.vote_yes -= asset(key_quantity.amount, STAKE_SYMBOL);
                    });
                }else{  
                    cases.modify(case_itr, account, [&]( auto& c){
                        c.vote_no -= asset(key_quantity.amount, STAKE_SYMBOL);
                    });
                }
            }
        }
    }

    if(st_itr == account_t.end()){
        voters.erase(voters_itr);
    }
}

void medishares::propose(account_name proposer, name case_name, asset required_fund){
    require_auth(proposer);
    eosio_assert(required_fund.amount > 0, "required_fund cannot be negative");
    eosio_assert(required_fund.symbol == CORE_SYMBOL, "this asset is not supported or the symbol precision mismatch");

    auto glb = global.begin();
    eosio_assert(glb != global.end(), "the global table does not exist");
    eosio_assert(glb->guarantee_pool.amount >= required_fund.amount, "can not require more than guarantee pool");
    global.modify(glb, 0, [&](auto& gl){
        gl.cases_num += 1;
    });

    accounts account_t(_self, proposer);

    //todo: 加入不可申请时间窗口机制，依据用户的加入时间
    auto itr = account_t.find(asset(0, CORE_SYMBOL).symbol.name());
    eosio_assert(itr != account_t.end(),"the user do not have guarantee balance");

    cases.emplace(proposer, [&](auto& c) {
        c.case_id = glb->cases_num;
        c.case_name = case_name;
        c.proposer = proposer;
        c.required_fund = required_fund;
        c.start_time = now();
        c.vote_yes = asset(0, STAKE_SYMBOL);
        c.vote_no = asset(0, STAKE_SYMBOL);
    });
}

void medishares::approve(account_name account, uint64_t case_id){
    require_auth(account);
    const auto& case_itr = cases.get(case_id, "case does not exist");
    eosio_assert(case_itr.start_time + TIME_WINDOW_FOR_VOTE >= now(), "out of time for vote");
    accounts account_t(_self, account);
    auto st_itr = account_t.get(asset(0,STAKE_SYMBOL).symbol.name(), "no stake balance object found");

    vote_entry vote_e;
    vote_e.case_id = case_id;
    vote_e.agreed = 1;
    auto voters_itr = voters.find(account);
    if(voters_itr == voters.end()){
        voters_itr = voters.emplace(account, [&](auto& v){
            v.voter = account;
            v.vote_list.push_back(vote_e);
        });
        cases.modify(case_itr, account, [&](auto& c){
            c.vote_yes += st_itr.balance;
        });
    }else{
        auto list_itr = std::find(voters_itr->vote_list.begin(), voters_itr->vote_list.end(), vote_e);
        if(list_itr != voters_itr->vote_list.end()){
            eosio_assert(list_itr->agreed != 1, "agreeded before");
            voters.modify(voters_itr, account, [&](auto& v){
                v.vote_list.erase(list_itr);
                v.vote_list.push_back(vote_e);
            });
            cases.modify(case_itr, account, [&](auto& c){
                c.vote_yes += st_itr.balance;
                c.vote_no -= st_itr.balance;
            });
        }else{
            voters.modify(voters_itr, account, [&](auto& v){
                v.vote_list.push_back(vote_e);
            });
            cases.modify(case_itr, account, [&](auto& c){
                c.vote_yes += st_itr.balance;
            });
        }
    }
}

void medishares::unapprove(account_name account, uint64_t case_id){
    require_auth(account);
    const auto& case_itr = cases.get(case_id, "case does not exist");
    eosio_assert(case_itr.start_time + TIME_WINDOW_FOR_VOTE >= now(), "out of time for vote");
    accounts account_t(_self, account);
    auto st_itr = account_t.get(asset(0, STAKE_SYMBOL).symbol.name(), "no stake balance object found");

    vote_entry vote_e;
    vote_e.case_id = case_id;
    vote_e.agreed = 0;
    auto voters_itr = voters.find(account);
    if(voters_itr == voters.end()){
        voters_itr = voters.emplace(account, [&](auto& v){
            v.voter = account;
            v.vote_list.push_back(vote_e);
        });

        cases.modify(case_itr, account, [&](auto& c){
            c.vote_no += st_itr.balance;
        });
    }else{
        auto list_itr = std::find(voters_itr->vote_list.begin(), voters_itr->vote_list.end(), vote_e);
        if(list_itr != voters_itr->vote_list.end()){
            eosio_assert(list_itr->agreed != 0, "unagreeded before");
            voters.modify(voters_itr, account, [&](auto& v){
                v.vote_list.erase(list_itr);
                v.vote_list.push_back(vote_e);
            });
            cases.modify(case_itr, account, [&](auto& c){
                c.vote_yes -= st_itr.balance;
                c.vote_no += st_itr.balance;
            });
        }else{
            voters.modify(voters_itr, account, [&](auto& v){
                v.vote_list.push_back(vote_e);
            });
            cases.modify(case_itr, account, [&](auto& c){
                c.vote_no += st_itr.balance;
            }); 
        }   
    }
}

void medishares::cancelvote(account_name account, uint64_t case_id){
    require_auth(account);
    const auto& case_itr = cases.get(case_id, "case does not exist");
    eosio_assert(case_itr.start_time + TIME_WINDOW_FOR_VOTE >= now(), "out of time for vote");
    auto voters_itr = voters.find(account);
    eosio_assert(voters_itr != voters.end(), "does not vote any cases");
    accounts account_t(_self, account);
    auto st_itr = account_t.get(STAKE_SYMBOL, "no stake balance object found");

    vote_entry vote_e;
    vote_e.case_id = case_id;
    vote_e.agreed = 10;
    auto list_itr = std::find(voters_itr->vote_list.begin(), voters_itr->vote_list.end(), vote_e);
    eosio_assert(list_itr != voters_itr->vote_list.end(), "does not vote this cases");
    if(list_itr->agreed == 1){
        cases.modify(case_itr, account, [&](auto& c){
            c.vote_yes -= st_itr.balance;
        });
    }else{
        cases.modify(case_itr, account, [&](auto& c){
            c.vote_no -= st_itr.balance;
        });
    }

    if(voters_itr->vote_list.size() > 1){
        voters.modify(voters_itr, account, [&](auto& v){
            v.vote_list.erase(list_itr);
        });
    }else{
        voters.erase(voters_itr);
    }
}

void medishares::execproposal(account_name account, uint64_t case_id){
    require_auth(account);
    auto case_itr = cases.get(case_id, "case does not exist");
    eosio_assert(case_itr.start_time + TIME_WINDOW_FOR_VOTE < now(), "voting has not been completed");
    auto glb = global.begin();
    eosio_assert(glb != global.end(), "the global table does not exist");
    const auto& market = keymarket.get(KEY_SYMBOL, "key market does not exist");
    eosio_assert(case_itr.vote_yes.amount * 100 >= market.supply.amount * PASS_THRESHOLD, "insufficient proportion of yes");

    auto vote_amount = (uint64_t)((double)case_itr.vote_yes.amount/(double)(case_itr.vote_yes.amount + case_itr.vote_no.amount)*case_itr.required_fund.amount);
    eosio_assert(vote_amount >= 1, "too little to transfer");
    eosio_assert(vote_amount <= glb->guarantee_pool.amount, "guarantee pool balance is not enough");
    string memo = std::string("case_id:"+std::to_string(case_itr.case_id)+", vote_yes:"+std::to_string(case_itr.vote_yes.amount)+"STKEY, vote_no:"+std::to_string(case_itr.vote_no.amount)+"STKEY, KEY supply:"+std::to_string(market.supply.amount)+"KEY, funding:"+std::to_string((double)vote_amount/10000)+"EOS");
    action(
        permission_level{_self, N(active)},
        N(eosio.token), N(transfer),
        std::make_tuple(_self, case_itr.proposer, asset(vote_amount, CORE_SYMBOL), memo)
    ).send();

    cases.erase(case_itr);

   //todo：扣减参与项目的账户的余额
}

/*
1.项目发起人可以随时删除项目；
2.过了投票期，若投票未通过，则所有人可以删除
*/
void medishares::delproposal(account_name account, uint64_t case_id){
    require_auth(account);
    auto case_itr = cases.get(case_id, "case does not exist");

    if(case_itr.proposer == account){
        cases.erase(case_itr);
        return;
    }

    eosio_assert(case_itr.start_time + TIME_WINDOW_FOR_VOTE < now(), "voting has not been completed");
    const auto& market = keymarket.get(KEY_SYMBOL, "key market does not exist");
    eosio_assert(case_itr.vote_yes.amount * 100 < market.supply.amount * PASS_THRESHOLD, "passed cases can not be deleted by others");

    cases.erase(case_itr);
}

