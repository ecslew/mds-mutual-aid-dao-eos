#include <functional>
#include <string>
#include <cmath>
#include <eosiolib/eosio.hpp>
#include <eosiolib/transaction.hpp>
#include <eosiolib/asset.hpp>

#define KEY_SYMBOL S(0,KEY)
#define STAKE_SYMBOL S(0,STKEY)

#define KEY_INIT_SUPPLY 1000000

#define TIME_WINDOW_FOR_VOTE ((uint64_t)(30*24*3600))
#define TIME_WINDOW_FOR_OBSERVATION ((uint64_t)(6*30*24*3600))

using namespace eosio;
using std::string;
using namespace std;

typedef double real_type;
struct transfer_args
{
    account_name from;
    account_name to;
    asset quantity;
    string memo;
};

class hbtcoop: public eosio::contract{
  public:
    hbtcoop(account_name self):
    contract(self),
    global(_self, _self),
    keymarket(_self, _self),
    cases(_self, _self),
    accounts(_self, _self)
    {}

    ///@abi action
    void init(const uint64_t guarantee_rate, const uint64_t ref_rate, asset max_claim);

    ///@abi action
    void transfer(account_name from, account_name to, asset quantity, string memo);

    ///@abi action
    void sellkey(account_name account, asset key_quantity);

    ///@abi action
    void stakekey(account_name account, asset key_quantity);

    ///@abi action
    void unstakekey(account_name account, asset key_quantity);

    ///@abi action
    void propose(account_name proposer, name case_name, asset required_fund);

    ///@abi action
    void approve(account_name account, uint64_t case_id);

    ///@abi action
    void unapprove(account_name account, uint64_t case_id);

    ///@abi action
    void cancelvote(account_name account, uint64_t case_id);

    ///@abi action
    void execproposal(account_name account, uint64_t case_id);

    ///@abi action
    void delproposal(account_name account, uint64_t case_id);

    inline asset get_balance(account_name owner, symbol_name sym)const;

    void handleTransfer(const account_name from, const account_name to, const asset& quantity, string memo);
	
  private:
    ///@abi table
    struct keymarket {
        asset    supply;

        struct connector {
            asset balance;
            double weight = .5;

            EOSLIB_SERIALIZE( connector, (balance)(weight) )
        };

        connector base;
        connector quote;

        uint64_t primary_key()const { return supply.symbol; }

        asset convert_to_exchange( connector& c, asset in );
        asset convert_from_exchange( connector& c, asset in );
        asset convert( asset from, symbol_type to );

        EOSLIB_SERIALIZE( keymarket, (supply)(base)(quote) )
    };

    eosio::multi_index<N(keymarket), keymarket> keymarket;

    struct asset_entry{
        asset    balance;          

        friend bool operator == ( const asset_entry& a, const asset_entry& b ) {
            return a.balance.symbol.name() == b.balance.symbol.name();
        }
    };

    bool has_balance(account_name owner, asset currency);
    void sub_balance(account_name owner, asset value);
    void add_balance(account_name owner, asset value, account_name ram_payer);

    struct vote_entry{
        uint64_t case_id;  
        uint8_t  agreed;   

        friend bool operator == ( const vote_entry& a, const vote_entry& b ) {
            return a.case_id == b.case_id;
        }
    };

    ///@abi table
    struct accounts {
        account_name    account;          
        time            join_time = 0;    
        vector<asset_entry> asset_list;   
        vector<vote_entry> vote_list;     

        uint64_t primary_key()const {return account;}

        EOSLIB_SERIALIZE(accounts, (account)(join_time)(asset_list)(vote_list));
    };

    eosio::multi_index<N(accounts), accounts> accounts;

    ///@abi table
    struct global
    {
        uint64_t     ref_rate;        
        uint64_t     guarantee_rate;  
        asset        guarantee_pool;  
        asset        bonus_pool;      
        uint64_t     cases_num;       
        uint64_t     applied_cases;   
        uint64_t     guaranteed_accounts;  
        asset        max_claim;       

        auto primary_key()const{return 0;}
        EOSLIB_SERIALIZE(global, (ref_rate)(guarantee_rate)(guarantee_pool)(bonus_pool)(cases_num)(applied_cases)(guaranteed_accounts)(max_claim))
    };
    eosio::multi_index<N(global), global> global;

    ///@abi table
    struct cases
    {
        uint64_t        case_id;        
        name            case_name;      
        account_name    proposer;       
        asset           required_fund;  
        time            start_time;     
        asset           vote_yes;       
	asset           vote_no;        
	
        auto primary_key()const{return case_id;}
        EOSLIB_SERIALIZE(cases, (case_id)(case_name)(proposer)(required_fund)(start_time)(vote_yes)(vote_no))
    };
    eosio::multi_index<N(cases), cases> cases;
};

extern "C"
{
    void apply(uint64_t receiver, uint64_t code, uint64_t action)
    {
        auto self = receiver;
        if( action == N(onerror)) { 
            /* onerror is only valid if it is for the "eosio" code account and authorized by "eosio"'s "active permission */ 
            eosio_assert(code == N(eosio), "onerror action's are only valid from the \"eosio\" system account"); 
        }

        hbtcoop thiscontract(self);
        if (code == self || action == N(onerror))
        {   // Action is pushed directly to the contract
            switch (action)
            {
                EOSIO_API(medishares, (init)(transfer)(sellkey)(stakekey)(unstakekey)(propose)(approve)(unapprove)(cancelvote)(execproposal)(delproposal))
            }
        }
        else if (code == N(eosio.token) && action == N(transfer))
        {   //receive message from eosio.token::transfer
            auto transferData = unpack_action_data<transfer_args>();
            if (transferData.to == self && transferData.from != N(eosio.ram) && transferData.from != N(eosio.stake))
            {    
                thiscontract.handleTransfer(transferData.from, transferData.to, transferData.quantity, transferData.memo);
            }
        }
        else
        {
            eosio_assert(false, "reject recepient from other contracts");
        }
    }
}

