#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <queue>
#include <iostream>
#include <ostream>
#include <fstream>
#include <sstream>//for stringstream
//#include <boost>//need to install boost
#include <vector>
using namespace std;

#define row 32
#define max_num_flows (row*row)
#define clock_ticks_per_byte 8

/*-------------------------------------------------------------------*/
/*------------------------ Sim Parameter: --------------------------*/
/*-------------------------------------------------------------------*/

/*-------------------------------------------------------------------*/
/*--------------------- variable collections: -----------------------*/
/*-------------------------------------------------------------------*/

//variable collections:
//slip state
struct Slip_State
{
	int next_dest[row];
	int next_src[row];
	int Tx_event[2+2*row];
	int contention_window;
	int cell_length;
	int header_length;
};

//struct for keeping track of TCP state
struct TCP_State
{
	int min_window;//in bytes
	int max_window;//in bytes
	int window[max_num_flows];//in bytes
	int first_sent[max_num_flows];//in bytes
	int last_sent[max_num_flows];//in bytes.
	int last_byte[max_num_flows];//total number of bytes generated so far for this flow
	int last_ack[max_num_flows];//should be sufficient since packets cannot arrive out of order.
	double mult_dec;//multiplicative decrement
	int max_seg;//in bytes
	double ema_delay[max_num_flows];//keeps track of average delay per queue
	double ema_par;//exponential moving average parameter
	int congestion_threshold;//if a queue exceeds this value mark it as congested with some probability
	double p_mark;//probability of marking the ecn_bit of a packet entering a congested queue
	int freeze_window_till[max_num_flows];//Need to ignore acks of packets sent after congestion is faced but before the window is decreased because we no longer drop packets.
};

//struct for storing the scheduler parameters.
struct Scheduler_Parameters
{
	int max_slip_its;// = 5;
	double alpha;// = .5;
	double beta;// = .1;
	double p_cap;// = .6;
	double avg_pkt_length;// = 100.0;
	int num_routers;//number of routers in the network.
};

//struct for storing simulation parameters
struct Simulation_Parameters
{
	int sched_type;// = 1;//ideal sim = 1//time slotted sim = 2//islip = 3
	bool use_tcp;//=false;//self explanatory
	bool use_markov_source;//use markov chains for generating traffic?
	bool all_pkts_are_same;//decides whether to return one size for all pkts or not
};
/*-------------------------------------------------------------------*/
/*--------------------- variable collections^ -----------------------*/
/*-------------------------------------------------------------------*/

/*-------------------------------------------------------------------*/
/*------------------------ Sim Parameters^^ -------------------------*/
/*-------------------------------------------------------------------*/


/*-------------------------------------------------------------------*/
/*--------------------- Class Declarations: -------------------------*/
/*-------------------------------------------------------------------*/

//Valuable Global Struct:
//a packet contains a packet id, a source, a destination, an arrival time, and an exit time;
struct Packet
{
	//	int packet_id;  //What is it's id number?
	int flow;//what flow does it belong to?
	int src;  //Where is it coming from
	int dest;  //Where is the packet it going
	int creation_time;  //When was the packet created
	int arrival_time;	//time arrived in the system.
	int length;  //length of packet
	bool is_not_dummy;//determines if packet should be counted or not
	int last_byte;//machinery to determine how to use tcp not used otherwise
	bool ecn_bit;//true if faced congestion along the path
};



//important global class:
class Event
{
public:
	int time;  //Time it takes place
	int Tx_start[2+2*row];//Tx_start[0]=time,Tx_start[1]=number of transmissions?,Tx_start[2+i*2]=ith source?, Tx_start[2+2*i+1]=ith dest?
	//only one at a time: specify s and d  toggles between transmitting and not
	int pkt_gen[max_num_flows];//as many as you like?
	int NIC_update[max_num_flows];//Start as many as you like.
	bool has_acks;//allows us to skip checking ack array constantly.  Perhaps useful for all the values.
	int ack[max_num_flows];//only the highest valued ack matters per flow.
	bool ecn_bit[max_num_flows];
	bool gen_state_changed;//allows us to ignore gen_state initialization
	int gen_state[max_num_flows];//markov state for generation
	//constructor: creates a blank event:
	Event()
	{
		time = -1;
		Tx_start[0]=-1;
		Tx_start[1]=0;
		has_acks=false;
		gen_state_changed=false;	
		for(int f=0;f<max_num_flows;f++)
		{
			pkt_gen[f]=0;
			NIC_update[f]=0;
		}
	}
	void set_time(int t)
	{
		time = t;
	};
	int get_time()
	{
		return time;
	};
	friend bool operator<(const Event& x, const Event& y) //priority is higher if number is smaller
	{
		if(x.time > y.time)
			return true;
		return false;
	};
	friend bool operator>(const Event& x, const Event& y) //priority is higher if number is smaller
	{
		if(x.time< y.time)
			return true;
		return false;
	};
	//combine this event with a concurrent event.
	void merge(Event *e)
	{
		if(e->get_time()<0)//invalid event
		{
			return;//do nothing
		}
		if(time<0||time>e->get_time())//other event is more recent
		{
			//simply copy all values
			time = e->get_time();
			for(int i = 0;i<2+2*row;i++)
			{
				Tx_start[i]=e->Tx_start[i];
			}
			for(int f =0;f<max_num_flows;f++)
			{
				pkt_gen[f]=e->pkt_gen[f];
				NIC_update[f]=e->NIC_update[f];
			}
			has_acks = e->has_acks;
			if(e->has_acks)//simply copy over all acks values
			{
				for(int f=0;f<max_num_flows;f++)
				{
					ack[f]=e->ack[f];
					ecn_bit[f]=e->ecn_bit[f];
				}
			}
			gen_state_changed = e->gen_state_changed;
			if(e->gen_state_changed)//simply copy over all acks values
			{
				for(int f=0;f<max_num_flows;f++)
				{
					this->gen_state[f]=e->gen_state[f];
				}
			}
		}
		else if(e->get_time()>time)//other event hasn't occurred yet.
		{
			return;//do nothing
		}
		else if(e->get_time()==time)//most important case: concurrent events
		{
			//transfer all the active differences
			if(e->Tx_start[1]>Tx_start[1])//the other event scheduled a larger feasible set
			{
				for(int i = 0;i<2+2*row;i++)
				{
					Tx_start[i]=e->Tx_start[i];
				}
			}
			for(int f =0;f<max_num_flows;f++)
			{
				if(e->pkt_gen[f]>=0)
				{
					if(pkt_gen[f]>=0)
					{
						pkt_gen[f]+=e->pkt_gen[f];//more were generated than you knew
					}
					else//pkt_gen[f]<0
					{
						pkt_gen[f]=e->pkt_gen[f];
					}
				}
				if(e->NIC_update[f]>NIC_update[f])
				{
					NIC_update[f]=e->NIC_update[f];//other event disagrees because of better info adding might not make sense
				}
			}
			if(e->has_acks)
			{
				if(has_acks)
				{
					for(int f=0;f<max_num_flows;f++)
					{
						ecn_bit[f]=ecn_bit[f]||e->ecn_bit[f];//acknowledge congestion whenever you find out?
						if((e->ack[f])>ack[f])//only the last received byte matters to the transmitter.
						{
							ack[f]=e->ack[f];
						}
					}
				}
				else//simply copy values:
				{
					has_acks = true;//signal you have acks now.
					for(int f=0;f<max_num_flows;f++)
					{
						ack[f]=e->ack[f];
						ecn_bit[f]=e->ecn_bit[f];
					}
				}
			}//if not we don't need to worry about it.
			if(e->gen_state_changed)
			{
				if(gen_state_changed)//merge values
				{
					//or the values as in keep all transitions for two state chains
					for(int f=0;f<max_num_flows;f++)
					{
						if(this->gen_state[f]==1||e->gen_state[f]==1)
						{
							this->gen_state[f]=1;
						}
						else
						{
							this->gen_state[f]=0;
						}
					}
				}
				else//simply copy values
				{
					for(int f=0;f<max_num_flows;f++)
					{
						this->gen_state[f]=e->gen_state[f];
					}
				}
			}//if not we don't need to worry about it.
		}
		
	};
};

//class for logging any errors or events that may occur during operation
//very simple to use: just initialize it with the file name of logs and record the event with the appropriate label
class Simple_Logger
{
	//time_t logger_timer;
	ofstream output;
	
	public:
	//constructor:
	//takes the output file name as an argument
	Simple_Logger(string output_file)
	{
		output.open(output_file.c_str());//need c string to open a file. 
	}
	
	//writes event to specified file in the format: [time in seconds] label: event
	void record(string label,string event)
	{
		output<<"["<<time(NULL)<<"] "<<label<<": "<<event<<"\n";//boost::posix_time::microsec_clock::local_time()
	}

	//writes event to specified file in the format: [time in Millis] label: event
	void record(string label,string event,int number)
	{
		output<<"["<<time(NULL)<<"] "<<label<<": "<<event<<" "<<number<<"\n";//boost::posix_time::microsec_clock::local_time()
	}
	
	//writes event to specified file in the format: [time in Millis] label: event
	void record(string label,string event,double number)
	{
		output<<"["<<time(NULL)<<"] "<<label<<": "<<event<<" "<<number<<"\n";//boost::posix_time::microsec_clock::local_time()
	}
};

//Data collector
//a class which aggregates data and then prints it to a file  Designed for easy use and to make data easily findable:
class Data_Collector
{
	vector<int> stat_type;//aggregation type
	int num_stats;
	vector<string> stat_name;
	vector<vector<int> > stat;
	vector<vector<int> > count;//not quite right at the moment...Need per time thing instead...
	vector<bool> count_is_user_defined;//allows user to ignore count values.
	vector<int> row_length;//formatting tool	
	public:
	//type variables to make it easy to use:
	static const int avg = 0;
	static const int max = 1;
	static const int variance = 2;
	//output modification:
	string preamble;//preface to the actual data.  Allows you to label things differently.
	//intialize 
	Data_Collector(int temp_num_stats)
	{
		num_stats=temp_num_stats;
		//int temp_stat_type[num_stats];
		//stat_type = temp_stat_type;
		stat_type.resize(num_stats);
		stat_name.resize(num_stats);
		stat.resize(num_stats);
		count.resize(num_stats);
		row_length.resize(num_stats);
		count_is_user_defined.resize(num_stats);
		preamble="";
	}

	int get_num_stats()
	{
		return num_stats; 
	}

	//original initialize_stat function will overload to make it more user friendly
	void initialize_stat(int this_stat,string name,int type,int num_indices,int stat_row_length,int init_value,bool use_count)
	{
		if(this_stat>get_num_stats()-1)
		{
			cout<<"stat size improperly initialized!\n";
			return;
			//increase_size...
			//shouldn't happen yet
			//corner case!
			//return;	
		}
		stat_type[this_stat]=type;
		stat_name[this_stat]=name;
		row_length[this_stat]=stat_row_length;
		count_is_user_defined[this_stat]=use_count;//.resize(num_indices);
		stat[this_stat].resize(num_indices);
		count[this_stat].resize(num_indices);
		for(int i=0;i<stat[this_stat].capacity();i++)
		{
			stat[this_stat][i]=init_value;
			count[this_stat][i]=0;
		}
	}

	//overloaded initialize_stat method to make it easy to use.
	void initialize_stat(int this_stat,string name,int type,int num_rows,int num_cols,bool user_defines_count)
	{
		initialize_stat(this_stat,name,type,num_rows*num_cols,num_cols,0,user_defines_count);//0 is probably the default value
	}

	//in order to avoid weird three layer arrays etc:
	//overloaded to allow for a more user friendly format
	void enter_data(int this_stat,int stat_index,int new_data)
	{
		//*
		int temp_stat=stat[this_stat][stat_index];//stat to be modified
		int temp_count=count[this_stat][stat_index];//current count to be modified
		//*
		switch(stat_type[this_stat])
		{
			case 0://average
				temp_stat+=new_data;//just add value;
				break;
			case 1://max
				if(temp_stat<new_data)
				{
					temp_stat=new_data;
				}
				break;
			case 2://variance
				temp_stat+=new_data*new_data;
				break;
			default://something went wrong
				temp_stat=-1;
				break;
		}//*/
		temp_count++;
		stat[this_stat][stat_index]=temp_stat;
		count[this_stat][stat_index]=temp_count;
		//*/
	}

	//overloaded enter_data method to allow user to think of data arrays as matrices
	void enter_data(int this_stat,int stat_row,int stat_col,int new_data)
	{
		if(stat_row*row_length[this_stat]+stat_col>=count[this_stat].capacity())
		{
			cout<<"error in overloaded entry data method\n";
			return;
		}
		enter_data(this_stat,stat_row*row_length[this_stat]+stat_col,new_data);
	}

	
	double single_stat(int this_stat,int specified_count)
	{
		double value=0;
		int temp_count;
		for(int i=0;i<stat[this_stat].size();i++)
		{
			temp_count = count[this_stat][i];
			if(count_is_user_defined[this_stat])
			{
				temp_count = specified_count; 
			}
			switch(stat_type[this_stat])
			{
				case 0://average
					value+=(stat[this_stat][i]*1.0/temp_count)/stat[this_stat].size();
					break;
				case 1://max
					if(value<stat[this_stat][i])
					{
						value=stat[this_stat][i];
					}
					break;
				case 2://variance
					value+=(stat[this_stat][i]*1.0/temp_count)/stat[this_stat].size();
					break;
				default://something went wrong
					value=-1;
					break;
			}
		}
		
	}
	
	//due to an error in bookkeeping use save_to_file_with_count() method below
	// Dump stored to statistics to the specified output file:
	void save_to_file(string file_name)
	{	
		ofstream output;	
		output.open(file_name.c_str());//need c string to open a file.
		for(int this_stat=0;this_stat<num_stats;this_stat++)
		{
			output<<stat_name[this_stat]<<":\n";
			for(int stat_index=0;stat_index<stat[this_stat].capacity();stat_index++)
			{
				if(count[this_stat][stat_index]!=0)
				{
					switch(stat_type[this_stat])
					{
						case 0://avg
							output<< ((double) stat[this_stat][stat_index]/count[this_stat][stat_index]);
							break;
						case 1://max
							output<<stat[this_stat][stat_index];
							break;
						case 2://variance:
							output<< ((double) stat[this_stat][stat_index]/count[this_stat][stat_index]);
							break;
						default://something is wrong
							output<<"type error";
					}
				}
				else
				{
					output<<0;
				}
				if(row_length[this_stat]>0&&row_length[this_stat]<=count[this_stat].capacity())
				{
					if((stat_index+1)%row_length[this_stat]==0)
					{
						output<<"\n";
					}
					else if(stat_index<count[this_stat].capacity()-1)
					{
						output<<" , ";
					}
				}
				else if(stat_index<count[this_stat].capacity()-1)
				{
					output<<" , ";
				}
			}
			if(row_length[this_stat]<=0||row_length[this_stat]>count[this_stat].capacity())
			{
				output<<"\n";
			}
		}
		output.close();
	}

	//hack to fix mistake in bookkeeping should be cleaned up later but not a priority at the moment.
	void save_to_file_with_count(string file_name,int real_count)
	{	
		ofstream output;	
		output.open(file_name.c_str());//need c string to open a file.
		for(int this_stat=0;this_stat<num_stats;this_stat++)
		{
			output<<stat_name[this_stat]<<":\n";
			for(int stat_index=0;stat_index<stat[this_stat].capacity();stat_index++)
			{
				if(real_count!=0)
				{
					switch(stat_type[this_stat])
					{
						case 0://avg
							output<< ((double) stat[this_stat][stat_index]/real_count);
							break;
						case 1://max
							output<<stat[this_stat][stat_index];
							break;
						case 2://variance:
							output<< ((double) stat[this_stat][stat_index]/real_count);
							break;
						default://something is wrong
							output<<"type error";
					}
				}
				else
				{
					output<<0;
				}
				if(row_length[this_stat]>0&&row_length[this_stat]<=stat[this_stat].capacity())
				{
					if((stat_index+1)%row_length[this_stat]==0)
					{
						output<<"\n";
					}
					else if(stat_index<stat[this_stat].capacity()-1)
					{
						output<<" , ";
					}
				}
				else if(stat_index<stat[this_stat].capacity()-1)
				{
					output<<" , ";
				}
			}
			if(row_length[this_stat]<=0||row_length[this_stat]>stat[this_stat].capacity())
			{
				output<<"\n";
			}
		}
		output.close();
	}

	

	//a further hack because I missed the idea of a variety of different kinds of statistics. 
	//Will need to fix this. Probbly doable if I change the data collection philosophy.
	// Dump stored to statistics to the specified output file:
	void save_to_file_specify_count_till(string file_name,int specified_count,int first_unspecified_count)
	{	
		int temp_count=0;
		ofstream output;	
		output.open(file_name.c_str());//need c string to open a file.
		for(int this_stat=0;this_stat<num_stats;this_stat++)
		{
			//commented out for now//output<<stat_name[this_stat]<<":\n";
			output<<stat[this_stat].capacity()/row_length[this_stat]<<","<<row_length[this_stat]<<"\n";
			for(int stat_index=0;stat_index<stat[this_stat].capacity();stat_index++)
			{
				if(this_stat<first_unspecified_count)
				{
					temp_count=specified_count;
				}
				else
				{
					temp_count=count[this_stat][stat_index];
				}
				if(temp_count!=0)
				{
					switch(stat_type[this_stat])
					{
						case 0://avg
							output<< ((double) stat[this_stat][stat_index]/temp_count);
							break;
						case 1://max
							output<<stat[this_stat][stat_index];
							break;
						case 2://variance:
							output<< ((double) stat[this_stat][stat_index]/temp_count);
							break;
						default://something is wrong
							output<<"type error";
					}
				}
				else
				{
					output<<0;
				}
				if(row_length[this_stat]>0&&row_length[this_stat]<=count[this_stat].capacity())
				{
					if((stat_index+1)%row_length[this_stat]==0)
					{
						output<<"\n";
					}
					else if(stat_index<count[this_stat].capacity()-1)
					{
						output<<" , ";
					}
				}
				else if(stat_index<count[this_stat].capacity()-1)
				{
					output<<" , ";
				}
			}
			if(row_length[this_stat]<=0||row_length[this_stat]>count[this_stat].capacity())
			{
				output<<"\n";
			}
		}
		output.close();
	}

	//Hopefully a save_to_file function that makes sense, pass in a file_name
	//and a specified count, for all statistics that have that flag ticked.
	void save_to_file_specify_count(string file_name,int specified_count)
	{
		int temp_count=0;
		ofstream output;	
		output.open(file_name.c_str());//need c string to open a file.
		for(int this_stat=0;this_stat<num_stats;this_stat++)
		{
			//commented out for now//output<<stat_name[this_stat]<<":\n";
			output<<stat[this_stat].capacity()/row_length[this_stat]<<","<<row_length[this_stat]<<"\n";
			for(int stat_index=0;stat_index<stat[this_stat].capacity();stat_index++)
			{
				if(count_is_user_defined[this_stat])
				{
					temp_count=specified_count;
				}
				else
				{
					temp_count=count[this_stat][stat_index];
				}
				if(temp_count!=0)
				{
					switch(stat_type[this_stat])
					{
						case 0://avg
							output<< ((double) stat[this_stat][stat_index]/temp_count);
							break;
						case 1://max
							output<<stat[this_stat][stat_index];
							break;
						case 2://variance:
							output<< ((double) stat[this_stat][stat_index]/temp_count);
							break;
						default://something is wrong
							output<<"type error";
					}
				}
				else
				{
					output<<0;
				}
				if(row_length[this_stat]>0&&row_length[this_stat]<=count[this_stat].capacity())
				{
					if((stat_index+1)%row_length[this_stat]==0)
					{
						output<<"\n";
					}
					else if(stat_index<count[this_stat].capacity()-1)
					{
						output<<" , ";
					}
				}
				else if(stat_index<count[this_stat].capacity()-1)
				{
					output<<" , ";
				}
			}
			if(row_length[this_stat]<=0||row_length[this_stat]>count[this_stat].capacity())
			{
				output<<"\n";
			}
		}
		output.close();
	}
	
	//ultimate version of this method (I hope).  Let's you use the specified count for the variables you flagged, 
	//furthermore you it prints out the preamble you've chosen specified in the future with whatever message you like before dumping the data.  (specify the preamble with data_collector_instance.preamble = some_string
	void dump_to_file(string file_name,int specified_count)
	{
		int temp_count=0;
		ofstream output;	
		output.open(file_name.c_str());//need c string to open a file.
		output<<preamble;
		for(int this_stat=0;this_stat<num_stats;this_stat++)
		{
			//commented out for now//output<<stat_name[this_stat]<<":\n";
			output<<stat[this_stat].capacity()/row_length[this_stat]<<","<<row_length[this_stat]<<"\n";
			for(int stat_index=0;stat_index<stat[this_stat].capacity();stat_index++)
			{
				if(count_is_user_defined[this_stat])
				{
					temp_count=specified_count;
				}
				else
				{
					temp_count=count[this_stat][stat_index];
				}
				if(temp_count!=0)
				{
					switch(stat_type[this_stat])
					{
						case 0://avg
							output<< ((double) stat[this_stat][stat_index]/temp_count);
							break;
						case 1://max
							output<<stat[this_stat][stat_index];
							break;
						case 2://variance:
							output<< ((double) stat[this_stat][stat_index]/temp_count);
							break;
						default://something is wrong
							output<<"type error";
					}
				}
				else
				{
					output<<0;
				}
				if(row_length[this_stat]>0&&row_length[this_stat]<=count[this_stat].capacity())
				{
					if((stat_index+1)%row_length[this_stat]==0)
					{
						output<<"\n";
					}
					else if(stat_index<count[this_stat].capacity()-1)
					{
						output<<" , ";
					}
				}
				else if(stat_index<count[this_stat].capacity()-1)
				{
					output<<" , ";
				}
			}
			if(row_length[this_stat]<=0||row_length[this_stat]>count[this_stat].capacity())
			{
				output<<"\n";
			}
		}
		output.close();
	}

	//overloaded for convenience:
	void dump_to_file(string file_name,int specified_count,string prefix)
	{
		preamble = prefix;
		dump_to_file(file_name,specified_count);
	}

	//ultimate version of this method (I hope).  Let's you use the specified count for the variables you flagged, 
	//furthermore you it prints out the preamble you've chosen specified in the future with whatever message you like before dumping the data.  (specify the preamble with data_collector_instance.preamble = some_string
	void dump(ostream* output,int specified_count)
	{
		bool aggregate=true;
		double agg_value=0;
		int temp_count=0;
		(*output)<<preamble;
		for(int this_stat=0;this_stat<num_stats;this_stat++)
		{
			//commented out for now//(*output)<<stat_name[this_stat]<<":\n";
			if(!aggregate)
			{
				(*output)<<stat[this_stat].capacity()/row_length[this_stat]<<","<<row_length[this_stat]<<"\n";
			}
			agg_value=0;	
			for(int stat_index=0;stat_index<stat[this_stat].capacity();stat_index++)
			{
				if(count_is_user_defined[this_stat])
				{
					temp_count=specified_count;
				}
				else
				{
					temp_count=count[this_stat][stat_index];
				}
				if(aggregate)
				{
					if(temp_count!=0)
					{
						switch(stat_type[this_stat])
						{
							case 0://avg
								agg_value+= ((double) stat[this_stat][stat_index]/stat[this_stat].capacity()/temp_count);
								break;
							case 1://max
								if(agg_value<stat[this_stat][stat_index])
								{
									agg_value=stat[this_stat][stat_index];
								}
								break;
							case 2://variance:
								agg_value+= ((double) stat[this_stat][stat_index]/stat[this_stat].capacity()/temp_count);
								break;
							default://something is wrong
								(*output)<<"type error";
						}
					}
				}
				else
				{
					if(temp_count!=0)
					{
						switch(stat_type[this_stat])
						{
							case 0://avg
								(*output)<< ((double) stat[this_stat][stat_index]/temp_count);
								break;
							case 1://max
								(*output)<<stat[this_stat][stat_index];
								break;
							case 2://variance:
								(*output)<< ((double) stat[this_stat][stat_index]/temp_count);
								break;
							default://something is wrong
								(*output)<<"type error";
						}
					}
					else
					{
						(*output)<<0;
					}
					if(row_length[this_stat]>0&&row_length[this_stat]<=count[this_stat].capacity())
					{
						if((stat_index+1)%row_length[this_stat]==0)
						{
							(*output)<<"\n";
						}
						else if(stat_index<count[this_stat].capacity()-1)
						{
							(*output)<<" , ";
						}
					}
					else if(stat_index<count[this_stat].capacity()-1)
					{
						(*output)<<" , ";
					}
				}
			}//for stat_index	
			if(aggregate)
			{
				(*output)<<stat_name[this_stat]<<": "<<agg_value<<"\n";
			}
			else
			{
				if(row_length[this_stat]<=0||row_length[this_stat]>count[this_stat].capacity())
				{
					(*output)<<"\n";
				}
			}
		}//for this_stat
	}


	//rezeros the Data_Collector's collected stats but leaves setup otherwise unchanged
	void reset()
	{
		for(int i=0;i<num_stats;i++)
		{
			for(int j=0;j<stat[i].capacity();j++)
			{
				stat[i][j]=0;
				count[i][j]=0;
			}
		}
	}
};

/*-------------------------------------------------------------------*/
/*--------------------- Class Declarations^^ ------------------------*/
/*-------------------------------------------------------------------*/

/*-------------------------------------------------------------------*/
/*--------------------- State Instantiation: ------------------------*/
/*-------------------------------------------------------------------*/

//Logging Class
Simple_Logger logger("logs/Event_Driven_Sim.log");

//Data Collection Class:
int flow_queue_avg = 0;
int flow_queue_var = 1;
int flow_queue_max = 2;
int switch_queue_avg = 3;
int switch_queue_var = 4;
int switch_queue_max = 5;
int packet_delay_avg = 6;
int packet_delay_var = 7;
int packet_delay_max = 8;
int tcp_window_avg = 9, tcp_window_max=10,tcp_sent_avg=11,tcp_sent_max=12;
Data_Collector stat_bucket(9+4);

struct Slip_State slip_state;
struct TCP_State tcp_state;
struct Scheduler_Parameters sched_par;
struct Simulation_Parameters sim_par;
//Declare some global varibles to simplify functions
//current time:
int current_time=0;

//TCP connections at the input:
int num_flows = max_num_flows;
queue<struct Packet> flow_Q[max_num_flows];
int flow_src[max_num_flows];
int flow_dest[max_num_flows];
double pkt_gen_rate[max_num_flows];
double on_2_off[max_num_flows];//prob of on to off transition
double off_2_on[max_num_flows];//probability of off to on transition
int gen_state[max_num_flows];//state variable
double tot_pkt_gen_rate = 0;
int NIC_state[max_num_flows];


//Switch Queues:
queue<struct Packet> switch_Q[row][row];

//Crossbar State:
int cbar_state[row][row];



//Event Heap that keeps track of most current event:
priority_queue<Event> event_heap; //event_heap

/*-------------------------------------------------------------------*/
/*--------------------- State Instantiation^^ -----------------------*/
/*-------------------------------------------------------------------*/

/*-------------------------------------------------------------------*/
/*-------------------------- TCP Machinery: -------------------------*/
/*-------------------------------------------------------------------*/

//functions for updating tcp window

//function that returns what to mark on the ecn_bit effective queue size let's you modify the scaling for slip schedulers for instance
bool new_ecn_value(int effective_queue_size)
{
	if(sim_par.use_tcp)
	{
		if(effective_queue_size>=tcp_state.congestion_threshold)
		{
			bool ecn_bit = (((double) rand())/INT_MAX)<tcp_state.p_mark;//mark true with probability p_mark
			if(ecn_bit)
			{
				logger.record("DEBUG","set ecn_bit for effective_queue_size= ",effective_queue_size);
			}
			return ecn_bit;
		}
	}
	return false;
}

//additive increase:
void inc_tcp_window(int flow)
{
	logger.record("debug","tcp window incremented by:",tcp_state.max_seg*tcp_state.max_seg/tcp_state.window[flow]);
	logger.record("debug","max seg : ",tcp_state.max_seg);
	logger.record("debug","tcp_state.window[flow]: ",tcp_state.window[flow]);
	tcp_state.window[flow] = tcp_state.window[flow]+tcp_state.max_seg*tcp_state.max_seg/tcp_state.window[flow];
	if(tcp_state.window[flow]>tcp_state.max_window)
	{
		tcp_state.window[flow]=tcp_state.max_window;
	}
}

//multiplicative decrease:
void dec_tcp_window(int flow)
{
	tcp_state.window[flow]=tcp_state.window[flow]*tcp_state.mult_dec;
	if(tcp_state.window[flow]<tcp_state.min_window)
	{
		tcp_state.window[flow]=tcp_state.min_window;
	}
}

//takes an event and does the tcp bookkeeping:
void tcp_update(Event *e)
{
	if(!e->has_acks)
	{
		return;
	}
	for(int f=0;f<max_num_flows;f++)
	{
		logger.record("debug","flow =",f);
		logger.record("debug","ack[f]=",e->ack[f]);
		logger.record("debug","tcp_state.first_sent[f]=",tcp_state.first_sent[f]);
		if(e->ack[f]>tcp_state.last_sent[f])
		{
			logger.record("ERROR","ack sent before pkt sent");
		}
		if((*e).ecn_bit[f])//congested!//need to make this explicit
		{
			logger.record("DEBUG","current_time = ",current_time);
			logger.record("DEBUG","ecn_bit true for flow = ",f);
			if(tcp_state.freeze_window_till[f]<=current_time)//check whether this is first ecn_bit in an RTT.
			{
				
				logger.record("DEBUG","ecn_bit taking effect for flow = ",f);
				logger.record("DEBUG","current_time = ",current_time);
				dec_tcp_window(f);
				logger.record("DEBUG","new tcp window = ",tcp_state.window[f]);
				tcp_state.freeze_window_till[f]=tcp_state.ema_delay[f]*sched_par.num_routers*2+current_time+1;//estimated RTT = delay_through_one_switch*num_switches_along_path*num_times_path_traversed
				logger.record("DEBUG","new freeze window till = ",tcp_state.freeze_window_till[f]);
			}
		}
		if(e->ack[f]>tcp_state.first_sent[f])
		{
			tcp_state.first_sent[f]=e->ack[f];
			if(!e->ecn_bit[f])//possibly not correct? What if there are two simultaneous acks?
			{
				if(tcp_state.freeze_window_till[f]<=current_time)//check whether this is first ecn_bit in an RTT.
				{
					inc_tcp_window(f);
				}
			}
			logger.record("debug","tcp flow: ",f);
			logger.record("debug","tcp window size increased: ",tcp_state.window[f]);
		}
	}
}

//functions governing use of sent:
//to determine if we can send more:
bool should_tcp_send(int flow)//perhaps should use a pointer to packet?
{
	if(flow_Q[flow].size()==0||NIC_state[flow]>0)
	{
		//logger.record("debug","NICState prevents transfer flow = ",NIC_state[flow]);
		return false;
	}
	struct Packet p = flow_Q[flow].front();
	return (p.length<=(tcp_state.window[flow]-(tcp_state.last_sent[flow]-tcp_state.first_sent[flow])));//true if there are enough bytes left in the window size
}

//decreases packet by delivered length.
void update_tcp_sent(int flow,int last_acked_byte)//perhaps use packet pointer?
{
	if(last_acked_byte>tcp_state.first_sent[flow])
	{
		tcp_state.first_sent[flow]+= last_acked_byte;
	}
}

//calculate ack based on round trip time across several routers
int ack_delay(int flow)
{
	int random_delay=0;//be something more formidable later.
	return sched_par.num_routers*tcp_state.ema_delay[flow]+random_delay;
}

//creates an ack event and pushes it onto the even heap:
void gen_ack(struct Packet *p)
{
	Event e;
	e.time = current_time+ack_delay((*p).flow);//?
	e.has_acks=true;
	for(int f=0;f<max_num_flows;f++)
	{
		e.ack[f]=0;
		e.ecn_bit[f]=false;
	}
	e.ack[p->flow] = p->last_byte;
	e.ecn_bit[p->flow] = p->ecn_bit;
	logger.record("debug","created ack with ecn_bit = ",p->ecn_bit);
	event_heap.push(e);//push onto heap?//?
}//*/

/*-------------------------------------------------------------------*/
/*-------------------------- TCP Machinery^ -------------------------*/
/*-------------------------------------------------------------------*/


/*-------------------------------------------------------------------*/
/*---------------------------- Schedulers: --------------------------*/
/*-------------------------------------------------------------------*/

//scheduler helper functions:
//writes to passed arrays the section of crossbar to run scheduler on:
//returns true if there is something to be scheduled
bool idle_c_bar(int *idle_src,int *idle_dest, int* num_idle_srcs_ptr, int* num_idle_dests_ptr)//,int *num_idle_srcs, int *num_idle_dests)
{
/*--------------------- Make into a function: -----------------------*/	
	//determine the idle subset of the crossbar:
	//int idle_src[row];
	//int idle_dest[row];
	int num_idle_srcs=0;
	int num_idle_dests=0;
	for(int i=0;i<row;i++)
	{
		//mark current src/dest as idle
		idle_src[num_idle_srcs]=i;
		idle_dest[num_idle_dests]=i;
		for(int j=0;j<row;j++)
		{
			if(cbar_state[i][j]>0)//src i is transmitting
			{
				idle_src[num_idle_srcs]=-1;
			}
			if(cbar_state[j][i]>0)//dest i is receiving
			{
				idle_dest[num_idle_dests]=-1;
			}
		}
		if(idle_src[num_idle_srcs]==i)//found an idle src!
		{
			num_idle_srcs++;
		}
		if(idle_dest[num_idle_dests]==i)//found an idle dest!
		{
			num_idle_dests++;
		}
	}
	
	//test that it is possible to transmit something:
	bool Tx_possible=false;
	for(int s=0;s<num_idle_srcs;s++)
	{
		for(int d=0;d<num_idle_dests;d++)
		{
			if(switch_Q[idle_src[s]][idle_dest[d]].size()>0)
			{
				Tx_possible=true;
				break;
			}
		}
	}
	/*--------------------- Make into a function^^ -----------------------*/	
	num_idle_srcs_ptr[0]=num_idle_srcs;
	num_idle_dests_ptr[0]=num_idle_dests;
	return Tx_possible;
}

//no probabilities yet:
//modifies three element transmission vector  grossly wrong at the moment
void basic_slotted_qcsma(int Tx_event[])
{
	//cout<<"basic_slotted_qcsma\n";
	//auxillary variables:
	double dec_var=0;

	
	/*--------------------- Make into a function: -----------------------*/	
	//determine the idle subset of the crossbar:
	int idle_src[row];
	int idle_dest[row];
	int num_idle_srcs=0;
	int num_idle_dests=0;
	/*
	for(int i=0;i<row;i++)
	{
		//mark current src/dest as idle
		idle_src[num_idle_srcs]=i;
		idle_dest[num_idle_dests]=i;
		for(int j=0;j<row;j++)
		{
			if(cbar_state[i][j]>0)//src i is transmitting
			{
				idle_src[num_idle_srcs]=-1;
			}
			if(cbar_state[j][i]>0)//dest i is receiving
			{
				idle_dest[num_idle_dests]=-1;
			}
		}
		if(idle_src[num_idle_srcs]==i)//found an idle src!
		{
			num_idle_srcs++;
		}
		if(idle_dest[num_idle_dests]==i)//found an idle dest!
		{
			num_idle_dests++;
		}
	}
	
	//test that it is possible to transmit something:
	int Tx_possible=0;
	for(int s=0;s<num_idle_srcs;s++)
	{
		for(int d=0;d<num_idle_dests;d++)
		{
			if(switch_Q[idle_src[s]][idle_dest[d]].size()>0)
			{
				Tx_possible=1;
				break;
			}
		}
	}//*/
	/*--------------------- Make into a function^^ -----------------------*/	
	/*for(int i=0;i<row;i++)
	{
		idle_dest[i]=-1;
		idle_src[i]=-1;
	}
	num_idle_dests=0;
	num_idle_srcs=0;//*/
	if(!idle_c_bar(idle_src,idle_dest,&num_idle_srcs,&num_idle_dests))
	{
	//if(Tx_possible==0)
	//{
		Tx_event[0]=-1;//time =-1;  return no event
		Tx_event[1]=0;
		return;
	}
	
	//do qcsma on idle crossbar:
	//request:
	int num_new_Tx=0;
	int request[row][row];
	int success[row];//to schedule on dest d;
	int contention_slots_used=0;
	double p[row][row];
	for(int s=0;s<num_idle_srcs;s++)
	{
		for(int d=0;d<num_idle_dests;d++)
		{
			if(switch_Q[idle_src[s]][idle_dest[d]].size()==0)
			{
				p[s][d] = 0.0;
			}
			else
			{
				p[s][d] = min(sched_par.p_cap,sched_par.beta*pow(switch_Q[idle_src[s]][idle_dest[d]].size(),sched_par.alpha));
			}
		}
	}
	while(num_new_Tx<=0)//protected against screwing up above
	{
		contention_slots_used++;
		if(contention_slots_used%1000==0)
		{
			logger.record("WARNING:","used a lot of contention slots current_time=",current_time);
			logger.record("WARNING:","used a lot of contention slots contention_slots_used=",contention_slots_used);
		}
		for(int d=0;d<num_idle_dests;d++)
		{
			success[d]=-1;//no requests
		}
		//make requests:
		for(int s=0;s<num_idle_srcs;s++)
		{
			for(int d=0;d<num_idle_dests;d++)
			{
				if((double)rand()/RAND_MAX<p[s][d])//prob of requesting function of source and dest
				{
					request[s][d]=1;
					if(success[d]==-1)//Only one request!
					{
						success[d]=s;
					}
					else//count conflicts:
					{
						if(success[d]>=0)
						{
							success[d]=-2;
						}
						else
						{
							success[d]--;
						}
					}
				}
				else
				{
					request[s][d]=0;
				}
			}
		}
		
		//check for collisions:
		//check for dest collisions
		for(int d=0;d<num_idle_dests;d++)
		{
			//cout<<"success["<<d<<"] = "<<success[d]<<"\n";
			if(success[d]==-1)//no transmission
			{
				//update probabilities... or not
			}
			if(success[d]<-1)//destination collision!
			{
				//update probabilities... or not
			}
			if(success[d]>=0)//we have a winner!
			{
				//check for source collisions and resolve them
				int count=0;
				int winner = success[d];//source that won!
				double total_backlog=0;
				for(int temp_d=d;temp_d<num_idle_dests;temp_d++)
				{
					if(success[temp_d] == winner)
					{
						count++;
						total_backlog+= switch_Q[idle_src[winner]][idle_dest[temp_d]].size();//total_backlog+= pow(switch_Q[idle_src[winner]][idle_dest[temp_d]].size(),1);//new
					}
				}
				//dec_var=((double)rand()/RAND_MAX)*count;//pick randomly?
				dec_var=((double)rand()/RAND_MAX)*total_backlog;//new
				//count = floor(dec_var);
				int conflict_resolved=0;
				//cout<<"count: "<<count<<"\n";
				for(int temp_d=d;temp_d<num_idle_dests;temp_d++)
				{
					if(success[temp_d]==winner)
					{
						dec_var-=switch_Q[idle_src[winner]][idle_dest[temp_d]].size();//dec_var-=pow(switch_Q[idle_src[winner]][idle_dest[temp_d]].size(),1);//new
						//if(count==0)
						if(dec_var<=0&&conflict_resolved==0)//new
						{
							Tx_event[2+2*num_new_Tx] = idle_src[winner];//set dest
							Tx_event[2+2*num_new_Tx+1] = idle_dest[temp_d];
							Tx_event[0] = current_time+contention_slots_used;
							num_new_Tx++;
							//cout<<"num_new_Tx after "<<contention_slots_used<<" contention_slots_used = "<<num_new_Tx<<"\n";
							//cout<<"current_time = "<<current_time<<"\n";
							Tx_event[1]=num_new_Tx;
							conflict_resolved=1;
						}
						success[temp_d]=-1;//May be inappropriate
						//count--;
					}
				}
			}
			
		}
		//update probabilities... or not
		//global p & temp p
		
		//if we have a winner return winner
		
		
		//if we don't have a winner repeat
		
	}
	
	
	//if((switch_Q[0][0].size()>0)&&cbar_state[0][0]<=0)
	//	{
	//		Tx_event[0]=current_time;//-1;//time
	//		Tx_event[1]=0;//-1;//source
	//		Tx_event[2]=0;//-1;//destination
	//	}
	//	else
	//	{
	//		Tx_event[0]=-1;//time
	//		Tx_event[1]=-1;//source
	//		Tx_event[2]=-1;//destination
	//		
	//	}
}


// ideal qcsma scheduler:
//checlk calculation of parameters
void ideal_qcsma(int Tx_event[])
{
	//find portion of c_bar to work on.
	int idle_src[row];
	int idle_dest[row];
	int num_idle_srcs;
	int num_idle_dests;
	if(!idle_c_bar(idle_src,idle_dest,&num_idle_srcs,&num_idle_dests))
	{
		Tx_event[0]=-1;//time =-1;  return no event
		Tx_event[1]=0;
		return;
	}
	
	double timer_rate[row][row];//time expiration rates
	double request_timer[row][row];//timers corresponding to different queues
	double min_request_timer = -1;//min value found
	int min_src=-1;
	int min_dest=-1;
	int num_min_requests=0;
	//generate requests
	for(int i=0;i<10;i++)//in case there is a zero probability event:
	{
		if(i>0)
		{
			logger.record("WARNING","rescheduling in ideal_qcsma!  Should not be happening");
		}
		min_request_timer = -1;//min value found
		min_src=-1;
		min_dest=-1;
		num_min_requests=0;
		for(int s=0;s<num_idle_srcs;s++)
		{
			for(int d=0;d<num_idle_dests;d++)
			{
				if(switch_Q[idle_src[s]][idle_dest[d]].size()==0)
				{
					timer_rate[s][d]=0;
					request_timer[s][d]=-1;//max double?
				}
				else
				{
					//rate
					timer_rate[s][d]=sched_par.beta*pow(switch_Q[idle_src[s]][idle_dest[d]].size(),sched_par.alpha);//not sure if this is the right expression to use
					//random variable:
					request_timer[s][d]=-log(1.0-((double) rand()/RAND_MAX))/timer_rate[s][d];//make it exponential -log(1-Y)/lambda;
					//is it the min?
					if(min_request_timer==request_timer[s][d])
					{
						num_min_requests++;
					}
					if(min_request_timer>request_timer[s][d])
					{
						min_request_timer= request_timer[s][d];
						min_src=s;
						min_dest=d;
						num_min_requests=1;
					}
					if(min_request_timer==-1)
					{
						min_request_timer = request_timer[s][d];
						min_src=s;
						min_dest=d;
						num_min_requests=1;
					}
				}
			}//d
		}//s
		//resolve conflicts
		if(num_min_requests==0)//no winner -- should never happen.
		{
			Tx_event[0]=-1;//time =-1;  return no event
			Tx_event[1]=0;
			logger.record("ERROR:","Error in ideal_qcsma nothing scheduled despite there being a transmission possible. Current time=",current_time);
			return;
		}
		if(num_min_requests==1)//Done!
		{
			Tx_event[0] = current_time+min_request_timer;
			Tx_event[1]=1;
			Tx_event[2+0] = idle_src[min_src];//set correct source dest pair
			Tx_event[2+1] = idle_dest[min_dest];
			return;					
		}
		if(num_min_requests>1)//in continuous time only one successful tranmissions should be allowed
		{
			//should be probability zero event. redo scheduling//still need code to catch this...
			//for now:
			Tx_event[0]=-1;//time =-1;  return no event
			Tx_event[1]=0;
			logger.record("WARNING:","There was a collision in ideal_qcsma nothing scheduled although this ought to be impossible. Current time=",current_time);
		}
	}
}


//islip helper algorithm:
void pkt_to_cells(struct Packet* pkt,queue<struct Packet>* next_pkt_queue,int cell_size,int header_length)
{
	/*struct Packet
	//	int packet_id;  //What is it's id number?
	int flow;//what flow does it belong to?
	int src;  //Where is it coming from
	int dest;  //Where is the packet it going
	int creation_time;  //When was the packet created
	int arrival_time;	//time arrived in the system.
	int length;  //length of packet*/
	if(pkt->length<=cell_size)//pkt already a cell
	{
		(*pkt).length=cell_size+header_length;//uniformize the packet lengths.
		next_pkt_queue->push(*pkt);
	}
	else//pkt too long
	{
		for(int i=0;i*cell_size<(*pkt).length;i++)
		{
			struct Packet p;//=new struct Packet;
			p.flow = pkt->flow;//arrival should not generate an ack or be counted towards statistics//pkt -> flow;
			p.src = pkt -> src;
			p.dest = pkt -> dest;
			p.creation_time = pkt -> creation_time;
			p.arrival_time = pkt -> arrival_time;
			p.length = cell_size+header_length;
			p.is_not_dummy = false;
			p.last_byte = pkt->last_byte;
			p.ecn_bit=pkt->ecn_bit;
			if((i+1)*cell_size>=(*pkt).length)//last element of the cell train should generate an ack
			{
				p.is_not_dummy = true;
			}
			(*next_pkt_queue).push(p);
		}
	}
}
//the slip algorithm:
void islip(int* Tx_event,int num_iterations)
{
	//check_state
	if(slip_state.Tx_event[0]>current_time)//next event has already been calculated?
	{
		//logger.record("Debug","islip has a premade event. current_time = ",current_time);
		for(int i=0;i<2*slip_state.Tx_event[1]+2;i++)
		{
			Tx_event[i]=slip_state.Tx_event[i];
			//logger.record("Debug","event to be returned:",Tx_event[i]);
		}
		return;
	}
	else if(current_time%(slip_state.cell_length+slip_state.header_length+slip_state.contention_window)!=0)//not time to schedule an event yet.
	{
		//logger.record("Debug","islip returns a schedule event. current_time = ",current_time);
		Tx_event[0]=(current_time/(slip_state.cell_length+slip_state.header_length+slip_state.contention_window)+1)*(slip_state.cell_length+slip_state.header_length+slip_state.contention_window);
		Tx_event[1]=0;//schedule a time to calculate the schedule --  Should line up with the crossbar finishing up anyways
		return;//wait for the appropriate time
	}
	if(current_time%(slip_state.cell_length+slip_state.header_length+slip_state.contention_window)!=0)//sanity check
	{
		logger.record("ERROR","islip scheduling although it is not the appropriate time.  current_time =",current_time);
	}
	int request[row][row];
	int src_sched[row];
	int dest_sched[row];
	int admit[row];
	int num_admissions[row];//per source
	int next=-1;//for round robin scheduling
	for(int i=0;i<row;i++)
	{
		src_sched[i]=-1;
		dest_sched[i]=-1;
		admit[i]=-1;
		num_admissions[i]=0;
	}
	for(int i =0;i<num_iterations;i++)
	{
		//generate requests
		for(int s=0;s<row;s++)
		{
			num_admissions[s]=0;//reset helper variables
			for(int d=0;d<row;d++)
			{
				request[s][d]=0;
				if(dest_sched[d]==-1&&src_sched[s]==-1)//src and dest not scheduled yet
				{
					if(switch_Q[s][d].size()>0)
					{
						request[s][d]=1;
						if(cbar_state[s][d]==0&&switch_Q[s][d].size()<=1)//the only packet in the queue will be transmitted now
						{
							request[s][d]=0;
						}
					}
				}
			}
		}
		//admit requests:
		for(int	d=0;d<row;d++)
		{
			admit[d]=-1;
			if(dest_sched[d]==-1)//not scheduled yet
			{
				for(int s=0;s<row;s++)
				{
					next=(slip_state.next_src[d]+s)%row;
					if(request[next][d]==1&&src_sched[next]==-1)
					{
						admit[d]=next;
						num_admissions[next]++;
						break;//skip out of the loop
					}
				}
			}
		}
		//accept requests:
		for(int	d=0;d<row;d++)
		{
			if(admit[d]!=-1&&dest_sched[d]==-1)//someone was admitted!
			{
				if(src_sched[admit[d]]==-1)//can accept!
				{
					if(num_admissions[admit[d]]>1||true) //check that I want to admit this one
					{
						for(int temp_d=0;temp_d<row;temp_d++)
						{
							next=(slip_state.next_dest[admit[d]]+temp_d)%row;
							if(src_sched[admit[d]]==-1&&dest_sched[next]==-1&&admit[next]==admit[d])
							{
								src_sched[admit[d]]=next;//schedule the admission
								dest_sched[next]=admit[d];
								break;
							}
						}
					}
					else//schedule a transmission:
					{
						src_sched[admit[d]]=d;
						dest_sched[d]=admit[d];
					}
				}
				else //src already scheduled.
				{
					admit[d]=-1;
				}
			}
		}
	//repeat num_iterations times
	}
	//return event and update state
	Tx_event[0]=current_time+slip_state.contention_window;//only perform calculation at the appropriate intervals
	Tx_event[1]=0;
	slip_state.Tx_event[0]=current_time+slip_state.contention_window;
	slip_state.Tx_event[1]=0;
	//logger.record("DEBUG","current_time = ",current_time);
	for(int s=0;s<row;s++)
	{
		if(src_sched[s]!=-1)
		{
			Tx_event[2+Tx_event[1]*2+0]=s;//src
			Tx_event[2+Tx_event[1]*2+1]=src_sched[s];//dest
			Tx_event[1]++;//increase number of Tx scheduled
			slip_state.Tx_event[2+slip_state.Tx_event[1]*2+0]=s;//src
			slip_state.Tx_event[2+slip_state.Tx_event[1]*2+1]=src_sched[s];//dest
			slip_state.Tx_event[1]++;//increase number of Tx scheduled
			//update pointers
			slip_state.next_dest[s]=(src_sched[s]+1)%row;
			slip_state.next_src[src_sched[s]]=(s+1)%row;
			//logger.record("DEBUG","Tx_event = "
		}
		//logger.record("DEBUG","next_dest dump=",slip_state.next_dest[s]);
	}
	if(Tx_event[1]==0)//no transmissions :>[
	{
		Tx_event[0]=-1;//no event
		slip_state.Tx_event[0]=-1;//no event
	}
}
/*-------------------------------------------------------------------*/
/*---------------------------- Schedulers^^ -------------------------*/
/*-------------------------------------------------------------------*/


/*-------------------------------------------------------------------*/
/*--------------------- Statistics Variables: -----------------------*/
/*-------------------------------------------------------------------*/

int pkts_delivered[row][row];//
int flow_pkts_delivered[max_num_flows];
int pkts_generated[max_num_flows];//
int tot_flow_delay[max_num_flows];
int peak_switch_Q[row][row];


/*-------------------------------------------------------------------*/
/*-------------------- Statistics Variables^^ -----------------------*/
/*-------------------------------------------------------------------*/

//return a random packet length drawn from some distribution
int pkt_length()
{	
	if(sim_par.all_pkts_are_same)
		return sched_par.avg_pkt_length;//2*slip_state.cell_length;
	//assume 12 bytes per slot
	if((double) rand()/INT_MAX<.5)//acks and control messages in response to a packet sent?
	{
		return 5;//64 bytes
		
	}
	else
	{
		return 120;//maxsize 1500 bytes? //((double)rand()/INT_MAX*95+25);//uniform between 300 and 1500 bytes
	}//return avg_pkt_length;//should be random
}



//updates state based on event information  Needs to have protections built in for invalid input:
void update_state(Event *event)
{
	int time_elapsed = event->get_time()-current_time;
	if(time_elapsed<0)  //nothing to do if the event occured in the past.
	{
		return;
	}
	for(int f=0;f<num_flows;f++)
	{
		//generate packets
		if(event->pkt_gen[f]!=0)
		{
			while(event->pkt_gen[f]>0)
			{
				Packet p;//generate packet: should be variable sized
				p.flow = f;
				p.src = flow_src[f];  //Where is it coming from
				p.dest = flow_dest[f];  //Where is the packet it going
				p.creation_time = event->get_time();  //When was the packet created
				p.arrival_time = -1;	//time arrived in the system.
				p.length = pkt_length();  //should be variable
				p.is_not_dummy=true;
				p.last_byte = -1;
				p.ecn_bit=false;
				if(sim_par.use_tcp)
				{
					tcp_state.last_byte[f]+=p.length;//keep track of data so far
					p.last_byte=tcp_state.last_byte[f];
				}
				pkts_generated[f]++;

				
				flow_Q[f].push(p);
				event->pkt_gen[f]--;
			}
		}
		
		//calculate transmissions to c_bar
		if(NIC_state[f]!=0)
		{
			NIC_state[f]-=time_elapsed;//update NIC_State
			if(NIC_state[f]==0&&(!flow_Q[f].empty()))//transmission finished
			{
				//cout<<"Switch Queue Loading\n";
				//cout<<"flow_Q[flow].empty() == "<<flow_Q[f].empty()<<"\n";
				Packet p = flow_Q[f].front();
				p.arrival_time = event->get_time();
				switch(sim_par.sched_type)//decide whether to break into cells or not
				{
					case 1://ideal qcsma
					case 2://slotted qcsma
						p.ecn_bit=new_ecn_value(switch_Q[p.src][p.dest].size());
						switch_Q[p.src][p.dest].push(p);
						break;
					case 3://iterative slip
						p.ecn_bit=new_ecn_value(switch_Q[p.src][p.dest].size()*slip_state.cell_length/sched_par.avg_pkt_length);//need to adjust queuelength to take number of cells into account.
						pkt_to_cells(&p,&(switch_Q[p.src][p.dest]),slip_state.cell_length,slip_state.header_length);
						//switch_Q[p.src][p.dest].push(p);
						break;
					default://incorrectly specified simulation type
						cout<<"Scheduler not appropriately specified change value of sched_type\n";
						logger.record("ERROR","Scheduler not appropriately specified change value of sched_type");
						break;
				}
				if(switch_Q[p.src][p.dest].size()>peak_switch_Q[p.src][p.dest])
				{
					peak_switch_Q[p.src][p.dest]=switch_Q[p.src][p.dest].size();
				}
				//cout<<"switch_Q["<<p.src<<"]["<<p.dest<<"].size() = "<<switch_Q[p.src][p.dest].size()<<"\n";
				flow_Q[f].pop();
				
			}
			if(event->NIC_update[f]==1&&(!flow_Q[f].empty()))
			{
				Packet p = flow_Q[f].front();
				NIC_state[f] = p.length;
				if(sim_par.use_tcp)
				{
					logger.record("debug","pkt just sent from flow: ",f);
					logger.record("debug","pkt.length: ",p.length);

					tcp_state.last_sent[f]+=p.length;
					//p.last_byte=tcp_state.last_sent[f];
					logger.record("debug","tcp_state.first_sent[f]: ",tcp_state.first_sent[f]);
					logger.record("debug","tcp_state.last_sent[f]: ",tcp_state.last_sent[f]);
					logger.record("debug","p.last_byte=",p.last_byte);
					stat_bucket.enter_data(packet_delay_max+1,p.flow,tcp_state.window[p.flow]);
					stat_bucket.enter_data(packet_delay_max+2,p.flow,tcp_state.window[p.flow]);
					stat_bucket.enter_data(packet_delay_max+3,p.flow,tcp_state.last_sent[p.flow]-tcp_state.first_sent[p.flow]);
					stat_bucket.enter_data(packet_delay_max+4,p.flow,tcp_state.last_sent[p.flow]-tcp_state.first_sent[p.flow]);
	//*/
				
				}
			}
		}
		if(NIC_state[f]==0)
		{
			NIC_state[f]=-1;
		}
	}
	
	//calculate crossbar transmission progress:
	for(int s=0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			if(cbar_state[s][d]!=0)
			{
				cbar_state[s][d]-=time_elapsed;
				if(cbar_state[s][d]==0&&!switch_Q[s][d].empty())//transmission success
				{
					Packet p = switch_Q[s][d].front();
					if(p.is_not_dummy)//make sure this pkt should contribute to the statistics
					{
						pkts_delivered[s][d]++;
						flow_pkts_delivered[p.flow]++;
						tot_flow_delay[p.flow] += current_time-p.creation_time;
						if(sim_par.use_tcp)
						{
							logger.record("debug","ema_delay = ",tcp_state.ema_delay[p.flow]);
							tcp_state.ema_delay[p.flow]=(current_time-p.arrival_time)+tcp_state.ema_par*tcp_state.ema_delay[p.flow];//ema(n) = delay + alpha*ema(n-1)
							
							logger.record("debug","flow = ",p.flow);
							logger.record("debug","ema_delay = ",tcp_state.ema_delay[p.flow]);
							logger.record("debug","exiting packets last_byte:",p.last_byte);
							gen_ack(&p);//?
						}
					}
					if(current_time-p.creation_time<0)
					{
						cout<<"negative delay!\n";
						cout<<"current_time = "<<current_time<<"\n";
						cout<<"p.creation_time = "<<p.creation_time<<"\n";
					}
					switch_Q[s][d].pop();//collect stats somehow
					if(p.is_not_dummy)//ignore garbage packets e.g. cells with flow =-1 that shouldn't count
					{
						stat_bucket.enter_data(packet_delay_avg,p.flow,current_time-p.creation_time);
						stat_bucket.enter_data(packet_delay_var,p.flow,current_time-p.creation_time);
						stat_bucket.enter_data(packet_delay_max,p.flow,current_time-p.creation_time);
					}
				}
			}
			if(cbar_state[s][d]==0)
			{
				cbar_state[s][d]=-1;
			}
		}
	}
	
	//Calculate new crossbar transmissions
	if(event->Tx_start[0]>=current_time&&event->Tx_start[1]>0)
	{
		for(int i=0;i<(event->Tx_start[1]);i++)
		{
			Packet p = switch_Q[event->Tx_start[2+i*2]][event->Tx_start[2+2*i+1]].front();
			cbar_state[event->Tx_start[2+i*2]][event->Tx_start[2+2*i+1]]=p.length;
			//generate_ack(event->Tx_start[2+2*i],event->Tx_start[2+2*i+1]);//generate ack
		}
	}
	
	if(event->gen_state_changed)
	{
		logger.record("DEBUG","gen_state_changed! current_time = ",current_time);
		for(int f=0;f<num_flows;f++)
		{
			gen_state[f]=event->gen_state[f];
		}
	}
	if(event->has_acks)
	{
		logger.record("DEBUG","has acks is true! current_time = ",current_time);
		tcp_update(event);
	}
	
	
	
	current_time = event->get_time();
	//push stuff onto the heap
}


/*-------------------------------------------------------------------*/
/*----------------------- Helper functions: -------------------------*/
/*-------------------------------------------------------------------*/

//returns true if printed an output
bool progress_bar(string label,int count,int tot_count,int divisor,time_t timer)//should eventually replace the mod five thing
{
	if(count*divisor%tot_count==0)
	{
		int time_elapsed = difftime(time(NULL),timer);
		cout<<"\nSimulated "<<count<<" events for "<<label<<" in "<<(time_elapsed/60)<<" minutes "<<(time_elapsed%60)<<" seconds. \n\n";
		return true;
	}
	return false;
}

void print(int array[], int array_length)
{
	for(int i = 0;i<array_length;i++)
	{
		cout<<array[i]<<" ";
	}
	cout<<"\n";
}

//helper function to dump the flow configuration to the specified file
//the format is: flow_num, row \n flow_sources \n flow_dest \n off_2_on \n on_2_off \n pkt_gen_rate \n
void dump_flows_to_file(string file_name)
{
	ofstream output;
	output.open(file_name.c_str());
	
	output<<num_flows<<","<<row<<","<<sched_par.avg_pkt_length<<"\n";
	
	for(int f=0;f<num_flows;f++)
	{
		output<<flow_src[f];
		if(f!=num_flows-1)
		{
			output<<",";
		}
	}
	output<<"\n";

	for(int f=0;f<num_flows;f++)
	{
		output<<flow_dest[f];
		if(f!=num_flows-1)
		{
			output<<",";
		}
	}
	output<<"\n";


	for(int f=0;f<num_flows;f++)
	{
		output<<off_2_on[f];
		if(f!=num_flows-1)
		{
			output<<",";
		}
	}
	output<<"\n";


	for(int f=0;f<num_flows;f++)
	{
		output<<on_2_off[f];
		if(f!=num_flows-1)
		{
			output<<",";
		}
	}
	output<<"\n";


	for(int f=0;f<num_flows;f++)
	{
		output<<pkt_gen_rate[f];
		if(f!=num_flows-1)
		{
			output<<",";
		}
	}
	output<<"\n";
}

void print(queue<Packet> (*Q)[row][row])
{
	for(int s=0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			cout<<(*Q)[s][d].size()<<"\t";
		}
		cout<<"\n";
	}
}

void print(queue<Packet> (*Q)[max_num_flows])
{
	for(int f=0;f<num_flows;f++)
	{
		cout<<(*Q)[f].size()<<"\t";
	}
	cout<<"\n";
}

void print(Event *e)
{
	cout<<"event:\n";
	cout<<"time = "<<e->time<<"\n";
	cout<<"Tx_start: ";
	if((e->Tx_start[1])>=0&&(e->Tx_start[1])<=row)
	{
		print(e->Tx_start,2*(e->Tx_start[1])+2);
	}
	cout<<"pkt_gen = ";
	print(e->pkt_gen,num_flows);
	cout<<"NIC_update = ";
	print(e->NIC_update,num_flows);
	cout<<"\n";
}


/*-------------------------------------------------------------------*/
/*----------------------- Helper functions^ -------------------------*/
/*-------------------------------------------------------------------*/



//intializae event to avoid weird occurences:
void init_event(Event *e)//should be superfluous with Event constructor...
{
	//didn't occur:
	e->time=-1;  
	//no transmission events:
	e->Tx_start[0]=-1;
	e->Tx_start[1]=0;
	for(int flow = 0;flow<max_num_flows;flow++)
	{
		e->pkt_gen[flow]=0;//no flows
		e->NIC_update[flow]=0;//no new NIC transmissions
	}
}

//Needs to schedule according to some scheduling policy:
Event next_Tx_start()
{
	Event e;
	init_event(&e);
	int Tx_event[2*row+2];//Tx_event[0] = time;//Tx_event[1]=num_Tx_started;//Tx_event[2]=s?//Tx_event[2+1] first event

	switch(sim_par.sched_type)
	{
		case 1://ideal qcsma
			ideal_qcsma(Tx_event);
			break;
		case 2://slotted qcsma
			basic_slotted_qcsma(Tx_event);
			break;
		case 3://iterative slip
			islip(Tx_event,sched_par.max_slip_its);
			break;
		default://incorrectly specified simulation type
			cout<<"Scheduler not appropriately specified change value of sched_type\n";
			logger.record("ERROR","Scheduler not appropriately specified change value of sched_type");
			break;
	}
	e.set_time(Tx_event[0]);
	for(int i=0;i<2+2*Tx_event[1];i++)
	{
		e.Tx_start[i]=Tx_event[i];
		//cout<<Tx_event[i]<<"\n";
		//e.Tx_start[1]=Tx_event[2];
	}
	//cout<<e.get_time()<<"\n";
	//cout<<"e.Tx_start[0] "<<e.Tx_start[0]<<"\n";
	//cout<<"e.Tx_start[1] "<<e.Tx_start[1]<<"\n";
	return e;
}

//calculates next transmission completion time:
Event next_cbar_done()
{
	Event e;
	init_event(&e);
	//nothing selected
	int s_min=-1;
	int d_min=-1;
	for(int s=0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			if(cbar_state[s][d]>=0&&!switch_Q[s][d].empty())
			{
				if(s_min==-1||d_min==-1)
				{
					s_min=s;
					d_min=d;
				}
				if(cbar_state[s][d]<cbar_state[s_min][d_min])
				{
					s_min=s;
					d_min=d;
				}
			}
		}
	}
	if(s_min!=-1&&d_min!=-1)
	{
		e.set_time(current_time+cbar_state[s_min][d_min]);
	}
	else
	{
		e.set_time(-1);
	}
	return e;
}

//
//Event next_cbar_scheduled()
//{
//	Event e;
//	init_event(&e);
//	
//	e.set_time(-1);
//	return e;
//}

//starts transferring if not being done already may need to be updated later
Event next_NIC_update()
{
	Event e;
	init_event(&e);
	int time=-1;
	//int NIC_update[max_num_flows];//S
	for(int flow =0;flow<num_flows;flow++)
	{
		e.NIC_update[flow]=0;
		if(NIC_state[flow]<=0)
		{
			if(sim_par.use_tcp)//check if tcp is being simulated 
			{
				if(should_tcp_send(flow))
				{

					logger.record("DEBUG","Tcp transfer started at current time = ",current_time);
					logger.record("DEBUG","Tcp transfer started for flow = ",flow);
				
					e.NIC_update[flow]=1;
					time = current_time;
				}
			}
			else
			{
				if(!flow_Q[flow].empty())
				{
					e.NIC_update[flow]=1;
					time = current_time;
				}
			}
		}
	}
	e.set_time(time);
	return e;
}

//finds next transfer completion time
Event next_NIC_transfer()
{
	Event e;
	init_event(&e);
	int flow_min=-1;
	for(int f =0;f<num_flows;f++)
	{
		if(NIC_state[f]>=0)
		{
			if(NIC_state[f]==0&&flow_Q[f].empty())//calls for immediate action but cannot be fulfilled so will block the situation
			{
				//do nothing.
			}
			else
			{
				if(flow_min == -1)
				{
					flow_min = f;
				}
				if(NIC_state[f]<NIC_state[flow_min])
				{
					flow_min = f;
				}
			}
		}
	}
	if(flow_min>-1)
	{
		e.set_time(current_time+NIC_state[flow_min]);
	}
	else
	{
		e.set_time(-1);
	}
	return e;
}

//returns an integer array containing the times the first of num_var geometric random variables expire
//with probability p.  If a random variable does not expire at the same time as the first one, it gets
//an entry of -1.
//p<0 interpreted as p=0, p>1 interpreted as p=1
int geo_exp(double* expiration_p,int* expired_var,int num_var)
{	
	if(num_var<0)
	{
		logger.record("ERROR","Invalid input into geo_exp.  num_var = ",num_var);
		num_var=0;//should create an error.  invalid input
	}
	//int expired_var[num_var];//could be zero length!//'return value'
	
	//calculate geometric probability that some variable expires:
	double p=1.0;
	for(int f=0;f<num_var;f++)
	{
		p = (1.0-expiration_p[f])*p;
	}
	p = 1.0-p;// p = 1 - prod(1-p_i)
	//cout<<"p = "<<p<<"\n";
	if(p<=0||p>1)
	{
		logger.record("ERROR","illegal probability in geo_exp with p = ",p);
		return -1;//expired_var;//some weird error occured.
	}
	
	//generate a uniform rand variable to be transformed to a geometric:
	double dec_var = ((double) rand())/INT_MAX;
	int time = 1;
	if(p!=1)
	{
		time = ceil(log(1.0-dec_var)/log(1.0-p));//time slot in which there is the first packet generated
	}
	//given at least one variable expired (E), did variable f generate one?
	bool var_expired=false;
	
	double p_no_exp=1;//pkt_gen_rate[0]/p;//p(x_1 = 1|E)
	for(int temp_f=0;temp_f<num_var;temp_f++)
	{
		dec_var = ((double) rand())/INT_MAX;//outcome
		int f = temp_f;//num_vars-1-temp_f;//
		if(var_expired)//the event in question already occured, so everything is independent :>]
		{
			if(dec_var<expiration_p[f])
			{
				expired_var[f]=1;//
			}
			else
			{
				expired_var[f]=0;
			}
		}
		else//no packets were generated yet :>/
		{
			if(dec_var<expiration_p[f]/(1-(1-p)/p_no_exp))//p_given_info)
			{
				expired_var[f]=1;//expired at time time
				var_expired=true;
			}
			else
			{
				expired_var[f]=0;//did not expire
				p_no_exp=p_no_exp*(1-expiration_p[f]);
			}
		}
	}
	return time;
}

//generates packets according to an on off markov process where packets are generated whenever the process is on.
// uses on_2_off prob, off_2_on prob and pkt_gen_on state
Event next_markov_pkt_gen()
{
	//double on_2_off[max_num_flows];//prob of on to off transition
	//double off_2_on[max_num_flows];//probability of off to on transition
	//bool gen_state[max_num_flows];//state variable
	Event e;
	//think this should be initialized by constructor?//init_event(&e);
	//transitions?
	double tran_p[max_num_flows];
	int did_transition[max_num_flows];
	double tran_time;
	for(int f=0;f<num_flows;f++)
	{
		tran_p[f]=off_2_on[f];
		if(gen_state[f]==1)//on
		{
			tran_p[f]=on_2_off[f];
		}
	}
	tran_time=geo_exp(tran_p,did_transition,num_flows);
	
	//pkt_gen?
	double gen_p[max_num_flows];
	int pkt_generated[max_num_flows];
	double gen_time;
	for(int f=0;f<num_flows;f++)
	{
		gen_p[f]=0;
		if(gen_state[f]==1)//on
		{
			gen_p[f]=pkt_gen_rate[f];
		}
	}
	gen_time=geo_exp(gen_p,pkt_generated,num_flows);

	//which occured first?
	int time=-1;
	if(tran_time>=0);//need to check for negative times 
	{
		time = tran_time;
	}
	if(time==-1||(time>gen_time&&gen_time>=0))
	{
		time=gen_time;
	}
	e.set_time(current_time+time);	
	if(time==tran_time)
	{
		e.gen_state_changed=true;
		for(int f=0;f<num_flows;f++)
		{
			e.gen_state[f]=gen_state[f];//no transition?
			if(did_transition[f]==1)
			{
				e.gen_state[f]=1-gen_state[f];//transition
			}
		}
	}
	if(time==gen_time)
	{
		for(int f=0;f<num_flows;f++)
		{
			e.pkt_gen[f]=pkt_generated[f];
		}
	}
	//return event
	
	return e;
}
//needs to be written:
Event next_pkt_gen()
{
	Event e;
	init_event(&e);
	
	//cout<<"entering next_pkt_gen function:\n";
	//calculate geometric probability that someone transmits:
	double p=1.0;
	for(int f=0;f<num_flows;f++)
	{
		p = (1.0-pkt_gen_rate[f])*p;
		//cout<<"p = "<<p<<"\n";
	}
	p = 1.0-p;// p = 1 - prod(1-p_i)
	//cout<<"p = "<<p<<"\n";
	if(p<=0||p>1)
	{
		return e;
	}
	
	//generate a uniform rand variable to be transformed to a geometric:
	double dec_var = ((double) rand())/INT_MAX;
	int time = 1;
	if(p!=1)
	{
		time = ceil(log(1.0-dec_var)/log(1.0-p));//time slot in which there is the first packet generated
	}
	
	//given at least one packet is generated (E), did flow f generate one?
	int pkt_was_generated=0;
	
	double p_no_pkts=1;//pkt_gen_rate[0]/p;//p(x_1 = 1|E)
	for(int temp_f=0;temp_f<num_flows;temp_f++)
	{
		dec_var = ((double) rand())/INT_MAX;//outcome
		int f = temp_f;//num_flows-1-temp_f;//
		if(pkt_was_generated==1)//the event in question already occured, so everything is independent :>]
		{
			if(dec_var<pkt_gen_rate[f])
			{
				e.pkt_gen[f]=1;//
			}
			else
			{
				e.pkt_gen[f]=0;
			}
		}
		else//no packets were generated yet :>/
		{
			if(dec_var<pkt_gen_rate[f]/(1-(1-p)/p_no_pkts))//p_given_info)
			{
				e.pkt_gen[f]=1;
				pkt_was_generated=1;
			}
			else
			{
				e.pkt_gen[f]=0;
				p_no_pkts=p_no_pkts*(1-pkt_gen_rate[f]);
			}
		}
	}
	//maybe exp(tot_pkt_gen_rate);
	//int packet_pos=0;//generated in proportion to the rates.
	//with probabilty packet_gen_rate[flow]/tot_pkt_gen_rate
	//cout<<"time = "<<time<<"\n";
	//cout<<"leaving next_pkt_gen function:\n";
	e.set_time(current_time+time);
	//for(int packet_pos=0;packet_pos<num_flows;packet_pos++)
	//	{
	//		e.pkt_gen[packet_pos]=1;
	//	}
	return e;
}


//time;  //Time it takes place
//int Tx_start[2];//only one at a time: specify s and d  toggles between transmitting and not
//int NIC_update[max_num_flows];//S

Event next_event()
{
	//cout<<"entering next_event\n";
	Event e;
	Event f;
	//*
	e = next_cbar_done();
	
	if(sim_par.use_markov_source)
	{
		f=next_markov_pkt_gen();
		e.merge(&f);
	}
	else//only consider iid pkt generation:
	{
		f = next_pkt_gen();
		e.merge(&f);
	}

	f=next_Tx_start();
	e.merge(&f);
	
	f = next_NIC_update();
	e.merge(&f);
	
	f = next_NIC_transfer();
	e.merge(&f);
	
	if(!event_heap.empty())//Check event_heap for any scheduled events
	{
		//cout<<heap not empty
		f = event_heap.top();
		while((!event_heap.empty())&&f.get_time()>=0&&(f.get_time()<=e.get_time()||e.get_time()<0))
		{
			logger.record("debug","event came off the heap! current_time = ",current_time);
			e.merge(&f);
			event_heap.pop();
			f = event_heap.top();
		}
	}
	//cout<<"exiting next_event\n";
	return e;//*/
}
//init_sim function.  Sets all the important parameters.
void init_sim(int log_num_events,double iid_load)
{
	//Parameters:
	sched_par.alpha = 1.0;
	sched_par.beta = 0.1;
	sched_par.p_cap = .6;
	sched_par.avg_pkt_length =.5*5+.5*120;//62.5;//52.75;//631.5;//150.0;
	sched_par.num_routers = 5;

	//Initialize state:
	num_flows = rand();
	//cout<<"num_flows: "<<num_flows<<"\n";
	//cout<<"((double)rand())/INT_MAX:"<<((double)rand())/INT_MAX<<"\n";
	num_flows = max_num_flows;//ceil(((double)rand())/INT_MAX*max_num_flows);
	cout<<"num_flows: "<<num_flows<<"\n";
	for(int s=0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			cbar_state[s][d] = 0;
		}
	}
	for(int f=0;f<num_flows;f++)
	{
		NIC_state[f]=0;
		flow_dest[f]=fmod(f,row);
		flow_src[f]=fmod(f/row,row);
		pkt_gen_rate[f]=iid_load/sched_par.avg_pkt_length/row;//generate packets every five hundred clockticks
		on_2_off[f]=.5;//eventually will be something
		off_2_on[f]=.5;//eventually will be something
		gen_state[f]=0;//eventually should startin steady state...
	}
	//Initialize statistics:
	for(int s = 0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			pkts_delivered[s][d]=0;
		}
	}
	
	for(int f=0;f<num_flows;f++)
	{
		flow_pkts_delivered[f]=0;
		tot_flow_delay[f]=0;
		pkts_generated[f]=0;
	}
	
	//initialize slip_state
	for(int i=0;i<row;i++)
	{
		slip_state.next_dest[i]=0;
		slip_state.next_src[i]=0;
	}
	slip_state.Tx_event[0]=-1;
	slip_state.Tx_event[1]=0;
	slip_state.cell_length=5;//length per cell in slip
	slip_state.header_length=1;
	slip_state.contention_window=1;//slip_state.cell_length/10;//contention window size

	//initialize tcp_state
	tcp_state.max_seg = 120;//1500 bytes per packet?
	tcp_state.min_window = tcp_state.max_seg;//totally arbitrary at the moment
	tcp_state.max_window = tcp_state.max_seg*30;//30 large packets per segment?
	tcp_state.mult_dec = .5;//arbitrary
	tcp_state.ema_par=.9;
	tcp_state.congestion_threshold=20;
	tcp_state.p_mark=0.5;
	for(int f=0;f<max_num_flows;f++)
	{
		tcp_state.window[f]=tcp_state.min_window;//?
		tcp_state.last_sent[f] = 0;
		tcp_state.first_sent[f] = 0;
		tcp_state.last_byte[f] = 0 ;
		tcp_state.last_ack[f]=0;//works as long as no packets can arrive out of order.
		tcp_state.ema_delay[f]=0;//exponential moving average.
		tcp_state.freeze_window_till[f]=0;
	}

	//Print parameters:
	cout<<"\n------------------- Simulation Parameters: ----------------------------\n\n";
	if(sim_par.use_tcp)
	{
		cout<<"using tcp with "<<sched_par.num_routers<<" routers.\n";
		cout<<"tcp_state.congestion_threshold = "<<tcp_state.congestion_threshold<<"\n";
		cout<<"tcp_state.p_mark = "<<tcp_state.p_mark<<"\n";
	}
	switch(sim_par.sched_type)//announce type of simulation
	{
		case 1://ideal qcsma
			cout<<"ideal qcsma\n";
			break;
		case 2://slotted qcsma
			cout<<"time slotted qcsma\n";
			break;
		case 3://iterative slip
			cout<<"iterative slip\n";
			cout<<"slip_state.cell_length = "<<slip_state.cell_length<<"\n";
		cout<<"slip_state.header_length = "<<slip_state.header_length<<"\n";
		cout<<"slip_state.contention_window = "<<slip_state.contention_window<<"\n";
			cout<<"max_slip_its = "<<sched_par.max_slip_its<<"\n";
			break;
		default://incorrectly specified simulation type
			cout<<"Scheduler not appropriately specified change value of sched_type\n";
			logger.record("ERROR","Scheduler not appropriately specified change value of sched_type");
			break;
	}
	cout<<"log_num_events = "<<log_num_events<<"\n";//1000000;
	cout<<"avg_pkt_length  = "<<sched_par.avg_pkt_length<<"\n";
	cout<<"alpha  = "<<sched_par.alpha<<"\n";
	cout<<"beta  = "<<sched_par.beta<<"\n";
	cout<<"p_cap  = "<<sched_par.p_cap<<"\n";
	cout<<"row = "<<row<<"\n";
	cout<<"max_num_flows = "<<max_num_flows<<"\n";
	cout<<"num_flows = "<<num_flows<<"\n";
	cout<<"iid_load = "<<iid_load<<"\n";


	cout<<"\n";
	
}

//Overload Init_sim to make calling it easier.
void init_sim(int log_num_events)
{
	init_sim(log_num_events,.4);
}
/*-------------------------------------------------------------------*/
/*---------------------------- Unit Tests: --------------------------*/
/*-------------------------------------------------------------------*/

//markov test:
//tests to see if markov source works correctly
void markov_source_test()
{
	int log_num_events = 5;
	int num_events = pow(10,log_num_events);//1000000;

	int pkt_count[max_num_flows];
	int on_count[max_num_flows];
	double on_load=1.0;
	double on_2_off_rate=.7;
	double off_2_on_rate=.3;
	init_sim(num_events,on_load);
	//zero everything out:
	for(int f=0;f<max_num_flows;f++)
	{
		on_count[f]=0;
		pkt_count[f] = 0;
		pkt_gen_rate[f]=0;
		on_2_off[f]=0;
		off_2_on[f]=0;	
	}
	for(int f=0;f<num_flows;f++)
	{
		pkt_gen_rate[f]=on_load/sched_par.avg_pkt_length/row;//generate packets every five hundred clockticks
		on_2_off[f]=on_2_off_rate;
		off_2_on[f]=off_2_on_rate;
	}
	double avg_on_time = off_2_on_rate/(off_2_on_rate+on_2_off_rate);
	

	Event e;
	for(int i=0;i<num_events;i++)
	{
		e = next_markov_pkt_gen();
		for(int f=0;f<num_flows;f++)
		{
			if(e.pkt_gen[f]!=0)
			{
				pkt_count[f]+=e.pkt_gen[f];
				e.pkt_gen[f]=0;
			}
			if(e.gen_state_changed)
			{
				on_count[f]+=e.gen_state[f];
			}
		}
		update_state(&e);
	}
	double sim_time_in_pkts=current_time*1.0/sched_par.avg_pkt_length; 
	for(int f=0;f<num_flows;f++)
	{
		cout<<"fraction of load generated = "<<pkt_count[f]/sim_time_in_pkts<<" on_count[f]/total_time="<<on_count[f]*1.0/current_time<<"\n";
	}
	cout<<"Expected time to be in on state: "<<avg_on_time<<"\n"; 
	cout<<"total sim time in pkt_lengths: "<<current_time*1.0/sched_par.avg_pkt_length<<"\n"; 
}


//tests if packet generation is iid uniform with a given load<1
//returns 1 if true;
double iid_pkt_gen_test(double load,int avg_length)
{
	
	double iid_load = load;
	//Parameters:
	sched_par.alpha = 1;
	sched_par.beta = .2;
	sched_par.p_cap = .8;
	sched_par.avg_pkt_length = avg_length;
	
	//Initialize state:
	num_flows = max_num_flows;//ceil(((double)rand())/INT_MAX*max_num_flows);
	for(int s=0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			cbar_state[s][d] = 0;
		}
	}
	for(int f=0;f<num_flows;f++)
	{
		NIC_state[f]=0;
		flow_dest[f]=fmod(f,row);
		flow_src[f]=fmod(f/row,row);
		pkt_gen_rate[f]=iid_load/sched_par.avg_pkt_length/row;//generate packets every five hundred clockticks
		//cout<<"pkt_gen_rate["<<f<<"] = "<<pkt_gen_rate[f]<<"\n";
	}
	//Initialize statistics:
	for(int s = 0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			pkts_delivered[s][d]=0;
		}
	}
	
	for(int f=0;f<num_flows;f++)
	{
		flow_pkts_delivered[f]=0;
		tot_flow_delay[f]=0;
		pkts_generated[f]=0;
	}
	
	//Print parameters:
	cout<<"\n------------------- Test Parameters: ----------------------------\n\n";
	cout<<"row = "<<row<<"\n";
	cout<<"max_num_flows = "<<max_num_flows<<"\n";
	cout<<"num_flows = "<<num_flows<<"\n";
	cout<<"iid_load = "<<iid_load<<"\n";
	cout<<"\n";
	
	Event e;
	int num_events=1000000;
	double avg_time_jump=0;
	double flow_jump_intervals[max_num_flows];
	int last_jump_time[max_num_flows];
	int num_jumps[max_num_flows];
	for(int f=0;f<num_flows;f++)
	{
		flow_jump_intervals[f]=0;
		last_jump_time[f]=0;
		num_jumps[f]=0;
	}
	//double avg_concurrent_gens=0;
	for(int count =0;count<num_events;count++)
	{
		e = next_pkt_gen();
		avg_time_jump+=e.get_time()-current_time;
		for(int f=0;f<num_flows;f++)
		{
			if(e.pkt_gen[f]==1)
			{
				flow_jump_intervals[f]+=e.get_time()-last_jump_time[f];
				last_jump_time[f]=e.get_time();
				num_jumps[f]++;
			}
		}
		update_state(&e);//current_time = e.get_time();
	}
	avg_time_jump=avg_time_jump/num_events;
	cout<<"avg_time_jump = "<<avg_time_jump<<"\n";
	
	
	cout<<"pkts_generated:\n";
	int tot_pkts_generated=0;
	double avg_pkts_per_event=0;
	for(int f=0;f<num_flows;f++)
	{
		cout<<pkts_generated[f]<<"\n";
		avg_pkts_per_event+=pkts_generated[f];
		tot_pkts_generated+=pkts_generated[f];
	}
	cout<<"avg_pkts_per_event = "<<avg_pkts_per_event/num_events<<"\n";
	cout<<"fraction of capacity generated: "<<tot_pkts_generated*sched_par.avg_pkt_length/row/current_time<<"\n";
	
	cout<<"avg_jump_interval:\n";
	for(int f=0;f<num_flows;f++)
	{
		cout<<flow_jump_intervals[f]/num_jumps[f]<<"\n";
	}
	cout<<"expected_time_jump = "<<1.0/pkt_gen_rate[0]<<"\n";
	
	cout<<"pkts_rates:\n";
	for(int f=0;f<num_flows;f++)
	{
		cout<<(double)pkts_generated[f]/current_time<<"\n";
	}
	
	cout<<"loading per row:\n";
	for(int f=0;f<num_flows;f++)
	{
		cout<<(double)pkts_generated[f]*sched_par.avg_pkt_length*row/current_time<<"\n";
	}
	
	cout<<"loading per row/desired load:\n";
	for(int f=0;f<num_flows;f++)
	{
		cout<<(double)pkts_generated[f]*sched_par.avg_pkt_length*row/current_time/load<<"\n";
	}
	cout<<"time_tested in clock ticks "<<current_time<<"\n";
	cout<<"time_tested in avg_pkt_lengths "<<current_time/sched_par.avg_pkt_length<<"\n";
	return tot_pkts_generated*sched_par.avg_pkt_length/row/current_time/load;
}

//Sanity Checks:
//checks if there are any queues above a certain value to make sure that we catch a saturation before it slows down the simulator unnecessarily
//returns true if some queue exceeds the specified value
bool switch_saturation_check(queue<Packet> (*Q)[row][row],int sat_limit)
{
	double size_scaling =1.0;
	if(sim_par.sched_type==3)//slip_state
	{
		size_scaling = sched_par.avg_pkt_length/slip_state.cell_length;
	}
	for(int s=0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			if((*Q)[s][d].size()>sat_limit*size_scaling)
			{
				cout<<"switch_Q["<<s<<"]["<<d<<"].size() = "<<(*Q)[s][d].size()<<"\n";
				//cout<<"at count "<<count<<" and time "<<current_time<<"\n";
				return true;
			}
		}
	}
	return false;
}

bool flow_saturation_check(queue<Packet> (*Q)[max_num_flows],int sat_limit)
{
	double size_scaling =1.0;
	if(sim_par.sched_type==3)//slip_state
	{
		size_scaling = sched_par.avg_pkt_length/slip_state.cell_length;
	}
	for(int f=0;f<max_num_flows;f++)
	{
		if((*Q)[f].size()>sat_limit*size_scaling)
		{
			cout<<"flow_Q["<<f<<"].size() = "<<(*Q)[f].size()<<"\n";
			return true;
		}
	}
	return false;
}

//checks the proposed transmission schedule to make sure the schedule is legal
//returns true if schedule is illegal
bool illegal_Tx_schedule_check()
{
	int dest_check[row];
	int src_check[row];
	for(int i=0;i<row;i++)
	{
		dest_check[i]=0;
		src_check[i]=0;
		for(int j=0;j<row;j++)
		{
			if(cbar_state[i][j]>0)
			{
				src_check[i]++;
				if(src_check[i]>1)
				{
					cout<<"illegal crossbar schedule due to source conflict!";
					return true;
					break;
				}
			}
			if(cbar_state[j][i]>0)
			{
				dest_check[i]++;
				if(dest_check[i]>1)
				{
					cout<<"illegal crossbar schedule due to destination conflict!";
					return true;
					break;
				}
			}
		}
	}
	return false;
}

void ptr_passing_test(struct Packet* parray)
{
	struct Packet p;
	p.flow = -1;//arrival should not generate an ack or be counted towards statistics//pkt -> flow;
	p.src =-1; 
	p.dest = -1;
	p.creation_time = -1;
	p.arrival_time = -1;
	p.length = -1;
	parray[0]=p;
}

//just a function to test whatever I just built without cluttering my main function
void test()
{
	string s1 = "hello ";
	string s2 = "world!";
	string s3 = s1+s2;
	cout<<s3<<"\n";
	
	stringstream temp;
	temp.str("");//empty string?
	string s4 = temp.str();//extract temp value?
	cout<< s4<<"\n";
	temp.str("reset string");
	string s5 = temp.str()+" " + s3;//extract temp value?
	cout<< s5<<"\n";

	temp.str("");//reset value to zero.
	temp<<"count ";//start filling buffer

	for(int i=0;i<5;i++)
	{
		temp<<":"<<i;
		cout<<temp.str()<<"\n";
	}
	/*	
	queue<Packet> pq;
	for(int i=0;i<6;i++)
	{
		struct Packet p;
		p.last_byte=i+1;
		pq.push(p);
	}
	struct Packet r=pq.front();
	cout<<r.last_byte<<"\n";
	r.last_byte=9;
	cout<<r.last_byte<<"\n";
	struct Packet q=pq.front();
	cout<<q.last_byte<<"\n";
	struct Packet parray;
	for(int i =0;i<row;i++)
	{
		ptr_passing_test(&parray);
		struct Packet p = parray;
		cout<<p.flow<<"\n";
		cout<<p.src<<"\n";
		cout<<p.dest<<"\n";
		cout<<p.creation_time<<"\n";
		cout<<p.arrival_time<<"\n";
		cout<<p.length<<"\n";
	}//*/
	/*
	int array_length=3;
	int* pointer;
	int place_holder[3];
	
	int array[array_length];
	for(int i=0;i<array_length;i++)
	{
		array[i]=array_length-i;
		place_holder[i]=array[i];
		cout<<"array["<<i<<"] = "<<array[i]<<"\n";
	}
	pointer=array;
	int* pointer_array[2];
	pointer_array[0]=pointer;
	pointer_array[1]=place_holder;
	for(int i=0;i<array_length;i++)
	{
		cout<<"(*pointer)["<<i<<"] = "<<(pointer)[i]<<"\n";
		(pointer)[i]=2*i;
	}
	for(int i=0;i<array_length;i++)
	{
		cout<<"array["<<i<<"] = "<<array[i]<<"\n";
	}
	for(int i=0;i<2;i++)
	{
		for(int j=0;j<3;j++)
		{
			cout<<pointer_array[i][j]<<" ";
		}
		cout<<"\n";
	}//*/
	/*
	Data_Collector d(2);
	int avg_flow_queue_size=0;
	cout<<"d.get_num_stats() = "<<d.get_num_stats()<<"\n";
	vector< string> v;
	v.resize(1);
	cout<<"v.capacity()="<< v.capacity()<<"\n";
	cout<<"v[0] ="<<v[0]<<"\n";
	v[0]="hello world!";
	cout<<"v[0] ="<<v[0]<<"\n";
	d.initialize_stat(avg_flow_queue_size,"avg flow queue size",d.avg,4,4);//initialize_stat(int this_stat,string name,int type,int num_indices,int init_value)
	//d.initialize_stat(1,"flow queue variance",2,12,4,0);//initialize_stat(int this_stat,string name,int type,int num_indices,int init_value)
	d.initialize_stat(1,"flow queue variance",d.variance,3,4);
	cout<<"d.get_num_stats() = "<<d.get_num_stats()<<"\n";
	for(int j=0;j<3;j++)
	{
		for(int i=0;i<12;i++)
		{
			d.enter_data(1,i,i+1);
		}
	}

	for(int j=0;j<4;j++)
	{
		for(int i=0;i<4;i++)
		{
			d.enter_data(avg_flow_queue_size,j,i,(i+1)*(j+1));
		}
	}
	d.save_to_file("logs/test.out");
	//*/
	/*
		struct Packet p = parray;
		cout<<p.flow<<"\n";
		cout<<p.src<<"\n";
		cout<<p.dest<<"\n";
		cout<<p.creation_time<<"\n";
		cout<<p.arrival_time<<"\n";
		cout<<p.length<<"\n";//*/
}
/*-------------------------------------------------------------------*/
/*---------------------------- Unit Tests^^ -------------------------*/
/*-------------------------------------------------------------------*/

/*-------------------------------------------------------------------*/
/*--------------------------- Experiments: --------------------------*/
/*-------------------------------------------------------------------*/

//function responsible to rezero all state so that a new simulation can begin
//does not change flows or other parameters, those should be done manually.
//needs to change array stats too?
void reset_sim()
{
	logger.record("debug","reset simulations");
	//empty out flow queues:
	for(int f=0;f<max_num_flows;f++)
	{
		while(!flow_Q[f].empty())
		{
			flow_Q[f].pop();
		}
	}
	//empty out switch queues:
	for(int s=0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			while(!switch_Q[s][d].empty())
			{
				switch_Q[s][d].pop();
			}
		}
	}
	//Initialize state:
	for(int s=0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			cbar_state[s][d] = 0;
		}
	}
	for(int f=0;f<num_flows;f++)
	{
		NIC_state[f]=0;
	}
	//Initialize statistics:
	for(int s = 0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			pkts_delivered[s][d]=0;
		}
	}
	
	for(int f=0;f<num_flows;f++)
	{
		flow_pkts_delivered[f]=0;
		tot_flow_delay[f]=0;
		pkts_generated[f]=0;
	}
	
	//initialize slip_state
	for(int i=0;i<row;i++)
	{
		slip_state.next_dest[i]=0;
		slip_state.next_src[i]=0;
	}
	slip_state.Tx_event[0]=-1;
	slip_state.Tx_event[1]=0;
	//slip_state.cell_length=5;//length per cell in slip
	//slip_state.header_length=1;
	//slip_state.contention_window=1;//slip_state.cell_length/10;//contention window size

	//initialize tcp_state
	for(int f=0;f<max_num_flows;f++)
	{
		tcp_state.window[f]=tcp_state.min_window;//?
		tcp_state.last_sent[f] = 0;
		tcp_state.first_sent[f] = 0;
		tcp_state.last_byte[f] = 0 ;
		tcp_state.last_ack[f]=0;//works as long as no packets can arrive out of order.
		tcp_state.ema_delay[f]=0;//exponential moving average.
		tcp_state.freeze_window_till[f]=0;
	}
	//empty event queue:
	while(!event_heap.empty())
	{
		event_heap.pop();
	}
	//reset current time:
	current_time=0;
	stat_bucket.reset();
}

//test the reset_sim function:
void test_reset_sim()
{
}

//run a simulation does not do any resetting or printing:
//not carefully examined yet
void run_sim(int num_events)
{
	time_t timer;
	time(&timer);
	Event e;
	
	cout<<"----------------------- Main Loop start: ------------------------------\n";
	//main simulation loop:
	bool end_simulation = false;
	//temp test variable:
	int max_last_Q=0;
	for(int count=0;count < num_events;count++)
	{
		if(progress_bar("Main Loop Execution",count,num_events,5,timer))
		{
			logger.record("Main Loop Execution","Number of iterations processed = ",count);
			logger.record("Main Loop Execution","Seconds Elapsed = ",difftime(time(NULL),timer));
			logger.record("Main Loop Execution","Current Sim Time = ",current_time);
			for(int f=0;f<max_num_flows;f++)
			{

				logger.record("DEBUG","flow:",f);
				logger.record("DEBUG","tcp_state.window:",tcp_state.window[f]);
				logger.record("DEBUG","tcp_state.waiting on ack:",tcp_state.last_sent[f]-tcp_state.first_sent[f]);
				logger.record("DEBUG","tcp_state.sent:",tcp_state.last_sent[f]);
			}
		}
		e = next_event();
		//collect statistics since last time:
		//*//record data (should go into a function?):
		//really slow at the moment
		int time_elapsed=e.get_time()-current_time;
		for(int flow=0;flow<max_num_flows;flow++)
		{
			stat_bucket.enter_data(flow_queue_avg,flow,flow_Q[flow].size()*time_elapsed);
			stat_bucket.enter_data(flow_queue_var,flow,flow_Q[flow].size()*time_elapsed);
			stat_bucket.enter_data(flow_queue_max,flow,flow_Q[flow].size());
		}
		for(int s=0;s<row;s++)
		{
			for(int d=0;d<row;d++)
			{
				stat_bucket.enter_data(switch_queue_avg,s,d,switch_Q[s][d].size()*time_elapsed);
				stat_bucket.enter_data(switch_queue_var,s,d,switch_Q[s][d].size()*time_elapsed);
				if(sim_par.sched_type==3)//slip need to rescale queues
				{
					stat_bucket.enter_data(switch_queue_max,s,d,switch_Q[s][d].size()*slip_state.cell_length/sched_par.avg_pkt_length);
				}
				else
				{
					stat_bucket.enter_data(switch_queue_max,s,d,switch_Q[s][d].size());
				}
			}
		}//*/
		
		update_state(&e);
		
		//instability check:
		end_simulation = end_simulation||switch_saturation_check(&switch_Q,300);
		end_simulation = end_simulation||flow_saturation_check(&flow_Q,2000);

		//sanity check:  see if crossbar has an illegal schedule
		end_simulation = end_simulation||illegal_Tx_schedule_check();
		if(end_simulation)
		{
			cout<<"simulation aborted at count "<<count<<" and time "<<current_time<<"\n";
			cout<<"switch queues:\n";
			print(&switch_Q);
			cout<<"flow queues:\n";
			print(&flow_Q);
			cout<<"max Q one step ago:\n";
			cout<<max_last_Q<<"\n";
			break;
		}
		else
		{
			max_last_Q=0;	
			for(int f=0;f<num_flows;f++)
			{
				if(flow_Q[f].size()>max_last_Q)
				{
					max_last_Q=flow_Q[f].size();
				}
			}
			for(int s=0;s<row;s++)
			{
				for(int d=0;d<row;d++)
				{
					if(max_last_Q<switch_Q[s][d].size())
					{
						max_last_Q=switch_Q[s][d].size();
					}
				}
			}

		}
	}
	
	cout<<"\n\n----------------------- Simulation Finished: -----------------------------\n\n";
	if(end_simulation)
	{
		cout<<"Simulation aborted after "<<difftime(time(NULL),timer)<<" seconds.\n";
	}
	cout<<"Program simulated "<<num_events<<" events in "<<difftime(time(NULL),timer)<<" seconds. ("<<difftime(time(NULL),timer)/60<<" minutes)\n";
	
}

//does a generates an increasing load of pkts and measure output
//does not use a tcp connection
void load_testing()
{
}

//searches for the best performing qcsma parameters:
void qcsma_parameter_search()
{
}

//tcp testing for different flow patterns:
void tcp_testing()
{
}

//generate high and low priority flows:
//in particular generate delay_sensitive percent delay sensitive flows
//and high_throughput high throughput flows
void dc_flow_pattern(double delay_sensitive,double high_throughput)
{
	sched_par.avg_pkt_length=.5*5+.5*120;//maybe wrong to put this here...?
	//high throughput parameters (taken from my previous simulators):
	double ht_on_2_off = .3;
	double ht_off_2_on = .7;
	double ht_gen_rate = 1.0/sched_par.avg_pkt_length/row;//generates packets at rate 1.0?

	//delay sensitive parameters:
	double ds_on_2_off = 1-5.0/(2*row);//.0781;
	double ds_off_2_on = 1-ds_on_2_off;//.9219;
	double ds_gen_rate = 1.0/sched_par.avg_pkt_length/row;//generates packets at rate 1?
	
	//decision variable:
	double dec_var=0.0;
	num_flows=0;
	for(int f=0;f<max_num_flows;f++)
	{
		dec_var=((double)rand()/RAND_MAX);//draw value of flow at random
		if(dec_var<delay_sensitive)//delay sensitive flow
		{
			//assign things to num_flows so we only need to check the first num_flows
			flow_dest[num_flows]=fmod(f,row);//destination is arbitrary
			flow_src[num_flows]=fmod(f/row,row);//source is arbitrary
			pkt_gen_rate[num_flows]=ds_gen_rate;//generate packets every five hundred clockticks
			on_2_off[num_flows]=ds_on_2_off;//eventually will be something
			off_2_on[num_flows]=ds_off_2_on;//eventually will be something
			gen_state[num_flows]=0;//eventually should startin steady state...*/
			
			num_flows++;//next one should not overlap
		}
		else if(dec_var<delay_sensitive+high_throughput)//high throughput flow
		{
			
			//assign things to num_flows so we only need to check the first num_flows
			flow_dest[num_flows]=fmod(f,row);//destination is arbitrary
			flow_src[num_flows]=fmod(f/row,row);//source is arbitrary
			pkt_gen_rate[num_flows]=ht_gen_rate;//generate packets every five hundred clockticks
			on_2_off[num_flows]=ht_on_2_off;//eventually will be something
			off_2_on[num_flows]=ht_off_2_on;//eventually will be something
			gen_state[num_flows]=0;//eventually should startin steady state...*/
			
			num_flows++;
		}
		else//no flow.
		{
			//f generates zero packets.
		}
	}
}

//assert function much like in JUnit 
bool assert_true(bool assertion, string error)
{
	if(assertion!=true)
	{
		cout<<error<<"\n";
		return false;
	}
	return true;
}

//test for dc_flow_pattern:
bool dc_flow_pattern_test()
{
	dc_flow_pattern(0,0);
	assert_true(num_flows==0,"num_flows!=0");

	double allowable_error=.1;//percentage you can bew off form the desired error
	double error=0.0;
	
	double ds_prob[18] = {.1,.2,.3,.4,.5,.6,.7,.8,.9,.1,.2,.3,.4,.5,.6,.1,.2,.3};
	double ht_prob[18] = {.9,.8,.7,.6,.5,.4,.3,.2,.1,.1,.2,.3,.3,.3,.3,.2,.2,.2};

	int num_ht=0;
	int num_ds=0;
	for(int i=0;i<18;i++)
	{
		cout<<"conducting dc_flow_pattern_test "<<i<<" :\n";
		dc_flow_pattern(ds_prob[i],ht_prob[i]);
		num_ds=0;
		num_ht=0;
		for(int f=0;f<num_flows;f++)
		{
			if(on_2_off[f]==.3&&off_2_on[f]==.7)
			{
				num_ht++;
			}
			if(on_2_off[f]==.9219&&off_2_on[f]==.0781)
			{
				num_ds++;
			}
		}
		//cout<<"ht_prob["<<i<<"] = "<<ht_prob[i]<<" percentage ht = "<<(num_ht*1.0/max_num_flows)<<"\n";
		//cout<<"ds_prob["<<i<<"] = "<<ds_prob[i]<<" percentage ht = "<<(num_ds*1.0/max_num_flows)<<"\n";
		//cout<<"\n";
		//*
		assert_true(num_flows==(num_ds+num_ht),"error in num_flow count");
		error = (num_ht*1.0/max_num_flows)-ht_prob[i];
		if(error<0)
		{
			error=error*-1;
		}
		if(!assert_true((error-allowable_error)<=0,"ht error too great!"))
		{
			cout<<"ht error for trial "<<i<<" is "<<error<<"\n";
		}
		error = num_ds*1.0/max_num_flows-ds_prob[i];
		if(error<0)
		{
			error=error*-1;
		}
		if(!assert_true(error-allowable_error<=0,"ds error too great!"))
		{
			cout<<"ds error for trial "<<i<<" is "<<error<<"\n";
		}//*/
		
		stringstream message;
		message.str("");//reset name
		message<<"output/"<<"flow"<<i<<".csv";//set name
			
		
		dump_flows_to_file(message.str());
	}
}


//tests whether my functions yield the same results as my main method.
void tcp_load_sim()
{
	cout<<"Entering tcp_load_sim\n";	
	//initialize stat collection mechanism:
	stat_bucket.initialize_stat(flow_queue_avg,"avg flow queues",stat_bucket.avg,1,max_num_flows,true);
	stat_bucket.initialize_stat(flow_queue_var,"flow queue variance",stat_bucket.variance,1,max_num_flows,true);
	stat_bucket.initialize_stat(flow_queue_max,"peak flow queues",stat_bucket.max,1,max_num_flows,true);
	stat_bucket.initialize_stat(switch_queue_avg,"avg switch queues",stat_bucket.avg,row,row,true);
	stat_bucket.initialize_stat(switch_queue_var,"switch queue variance",stat_bucket.variance,row,row,true);
	stat_bucket.initialize_stat(switch_queue_max,"peak switch queues",stat_bucket.max,row,row,true);

	//packet delay statistics:
	stat_bucket.initialize_stat(packet_delay_avg,"avg packet delays",stat_bucket.avg,1,max_num_flows,false);
	stat_bucket.initialize_stat(packet_delay_var,"packet delay variance",stat_bucket.variance,1,max_num_flows,false);
	stat_bucket.initialize_stat(packet_delay_max,"max packet delay",stat_bucket.max,1,max_num_flows,false);
	
	//tcp debugging data:
	stat_bucket.initialize_stat(packet_delay_max+1,"avg tcp window size",stat_bucket.avg,1,max_num_flows,false);
	stat_bucket.initialize_stat(packet_delay_max+2,"max tcp window size",stat_bucket.max,1,max_num_flows,false);
	stat_bucket.initialize_stat(packet_delay_max+3,"avg tcp sent",stat_bucket.avg,1,max_num_flows,false);
	stat_bucket.initialize_stat(packet_delay_max+4,"max tcp sent",stat_bucket.max,1,max_num_flows,false);
	//Parameters:
	sim_par.use_tcp=true;
	sim_par.use_markov_source=true;
	sim_par.sched_type = 1;
	sched_par.max_slip_its=6;
	int log_num_events = 6;
	int num_events =pow(10,log_num_events);//1000000;

	//run trials:
	string type_name="";
	stringstream message;
	double max_load =.99;
	double load_array[10]={.1,.3,.5,.6,.7,.75,.8,.85,.9,.95};
	double load;

	for(int type = 1;type<=3;type++)//do different types of simulations
	{	
		sim_par.sched_type = type;
		switch(sim_par.sched_type)
		{
			case 1://ideal qcsma
			//set parameters correctly?  maybe not since we call init_sim below...
				type_name="ideal_qcsma";
				tcp_state.p_mark=.9;//from old simulator
				tcp_state.congestion_threshold=10;//from old simulator
				break;
			case 2://slotted qcsma
				type_name="slotted_qcsma";
				tcp_state.p_mark=.5;//from old simulator
				tcp_state.congestion_threshold=12;//from old simulator
				break;
			case 3://iterative slip
				type_name="slip";
				tcp_state.p_mark=.5;//from old simulator
				tcp_state.congestion_threshold=10;//from old simulator
				break;
			default://error has occurred
				cout<<"Error incorrect type in tcp_load_sim\n";
				return;
		}
		//tcp_state.p_mark=0;//temp test definitely remove
		//tcp_state.congestion_threshold=12;//from old simulator
	
		for(int i=1;i<=1;i++)
		{
			//initialize_state:
			load = load_array[i-1];//.1*i*max_load;
			init_sim(log_num_events,load);
			
			dc_flow_pattern(.8/row,.2/row);
			//slip_state.cell_length = sched_par.avg_pkt_length;//temp fix'
			slip_state.header_length = 0;
			sched_par.beta=.1;
			sched_par.alpha=.5;
			sched_par.p_cap=.2;
			sched_par.max_slip_its=6;

			reset_sim();		
			
			//run simulation:
			run_sim(num_events);

			//save to file:
			message.str("");//reset preamble
			message<<"1,2\n"<<load<<","<<sched_par.avg_pkt_length<<"\n";//add load to label
			stat_bucket.preamble = message.str();//set preamble

			message.str("");//reset name
			message<<"output/tcp_"<<type_name<<i<<".csv";//set name
			stat_bucket.dump_to_file(message.str(),current_time);//write file
			
			message.str("");
			message<<"output/tcp_"<<type_name<<"_flow"<<i<<".csv";
			dump_flows_to_file(message.str());
			
			stat_bucket.dump(&cout,current_time);
			//stat_bucket.save_to_file_specify_count(message.str(),current_time);//write file
		}
	}
}


//tests whether my functions yield the same results as my main method.
void iid_load_sim()
{
	cout<<"Entering iid_load_sim\n";	
	//initialize stat collection mechanism:
	stat_bucket.initialize_stat(flow_queue_avg,"avg flow queues",stat_bucket.avg,1,max_num_flows,true);
	stat_bucket.initialize_stat(flow_queue_var,"flow queue variance",stat_bucket.variance,1,max_num_flows,true);
	stat_bucket.initialize_stat(flow_queue_max,"peak flow queues",stat_bucket.max,1,max_num_flows,true);
	stat_bucket.initialize_stat(switch_queue_avg,"avg switch queues",stat_bucket.avg,row,row,true);
	stat_bucket.initialize_stat(switch_queue_var,"switch queue variance",stat_bucket.variance,row,row,true);
	stat_bucket.initialize_stat(switch_queue_max,"peak switch queues",stat_bucket.max,row,row,true);

	//packet delay statistics:
	stat_bucket.initialize_stat(packet_delay_avg,"avg packet delays",stat_bucket.avg,1,max_num_flows,false);
	stat_bucket.initialize_stat(packet_delay_var,"packet delay variance",stat_bucket.variance,1,max_num_flows,false);
	stat_bucket.initialize_stat(packet_delay_max,"max packet delay",stat_bucket.max,1,max_num_flows,false);
	
	//Parameters:
	sim_par.use_tcp=false;
	sim_par.sched_type = 1;
	sched_par.max_slip_its=7;
	int log_num_events = 5;
	int num_events =pow(10,log_num_events);//1000000;

	//run trials:
	string type_name="";
	stringstream message;
	double load_array[10]={.1,.3,.5,.6,.7,.75,.8,.85,.9,.95};
	double load;
	for(int type = 1;type<=3;type++)//do different types of simulations
	{	
		sim_par.sched_type = type;
		switch(sim_par.sched_type)
		{
			case 1://ideal qcsma
			//set parameters correctly?  maybe not since we call init_sim below...
				type_name="ideal_qcsma";
				break;
			case 2://slotted qcsma
				type_name="slotted_qcsma";
				break;
			case 3://iterative slip
				type_name="slip";
				break;
			default://error has occurred
				cout<<"Error incorrect type in iid_load_sim\n";
				return;
		}
		for(int i=1;i<=10;i++)
		{
			//initialize_state:
			load = load_array[i-1];//.1*i*max_load;
			init_sim(log_num_events,load);
			//slip_state.cell_length = sched_par.avg_pkt_length;//temp fix'
			slip_state.header_length = 0;
			sched_par.beta=.1;
			sched_par.alpha=.5;
			sched_par.p_cap=.2;
			sched_par.max_slip_its=6;

			reset_sim();		
			
			//run simulation:
			run_sim(num_events);

			//save to file:
			message.str("");//reset preamble
			message<<"1,2\n"<<load<<","<<sched_par.avg_pkt_length<<"\n";//add load to label
			stat_bucket.preamble = message.str();//set preamble

			message.str("");//reset name
			message<<"output/"<<type_name<<i<<".csv";//set name
			stat_bucket.dump_to_file(message.str(),current_time);//write file
			
			message.str("");
			message<<"output/"<<type_name<<"_flow"<<i<<".csv";
			dump_flows_to_file(message.str());
			
			stat_bucket.dump(&cout,current_time);
			//stat_bucket.save_to_file_specify_count(message.str(),current_time);//write file
		}
	}
}

//search for best QCSMA parameters:
void qcsma_par_search()
{
	
	cout<<"Entering qcsma_par_search\n";	
	//initialize stat collection mechanism:
	stat_bucket.initialize_stat(flow_queue_avg,"avg flow queues",stat_bucket.avg,1,max_num_flows,true);
	stat_bucket.initialize_stat(flow_queue_var,"flow queue variance",stat_bucket.variance,1,max_num_flows,true);
	stat_bucket.initialize_stat(flow_queue_max,"peak flow queues",stat_bucket.max,1,max_num_flows,true);
	stat_bucket.initialize_stat(switch_queue_avg,"avg switch queues",stat_bucket.avg,row,row,true);
	stat_bucket.initialize_stat(switch_queue_var,"switch queue variance",stat_bucket.variance,row,row,true);
	stat_bucket.initialize_stat(switch_queue_max,"peak switch queues",stat_bucket.max,row,row,true);

	//packet delay statistics:
	stat_bucket.initialize_stat(packet_delay_avg,"avg packet delays",stat_bucket.avg,1,max_num_flows,false);
	stat_bucket.initialize_stat(packet_delay_var,"packet delay variance",stat_bucket.variance,1,max_num_flows,false);
	stat_bucket.initialize_stat(packet_delay_max,"max packet delay",stat_bucket.max,1,max_num_flows,false);
	
	//Parameters:
	sim_par.use_tcp=false;
	sim_par.use_markov_source=false;
	sim_par.sched_type = 2;//time slotted qcsma 
	sched_par.max_slip_its=10;//doesn't matter.
	int log_num_events = 6;
	int num_events = pow(10,log_num_events);//1000000;

	//search parameters:
	double benchmark_load =.7;//targetting good performance for this load
	vector<double> alpha;
	alpha.push_back(0.5);
	alpha.push_back(1.0);
	alpha.push_back(2.0);
	vector<double> beta;
	beta.push_back(0.1);
	beta.push_back(0.6);
	beta.push_back(1.0);
	vector<double> p_cap;
	//p_cap.push_back(0.1);
	//p_cap.push_back(0.3);
	p_cap.push_back(0.6);//perhaps too large... Want to search such that it can always generate a schedule in a reasonable time.
	
	//run trials:
	stringstream message;
	double best_delay=-1;//nothing found yet.
	double best_alpha=0;
	double best_beta=0;
	double best_p_cap=0;
	double current_delay=-1;//nothing found yet.
	for(int a=0;a<alpha.size();a++)
	{
		for(int b=0;b<beta.size();b++)
		{
			for(int p=0;p<p_cap.size();p++)
			{
				cout<<"\n\n";
				cout<<"alpha = "<<alpha[a]<<" --- ";
				cout<<"beta = "<<beta[b]<<" --- ";
				cout<<"p_cap = "<<p_cap[p]<<"\n";
				cout<<"\n\n";

				//initialize_state:
				init_sim(log_num_events,benchmark_load);
				sched_par.alpha = alpha[a];
				sched_par.beta = beta[b];
				sched_par.p_cap = p_cap[p];
				reset_sim();
				
				//run simulation:
				run_sim(num_events);

				//save to file:
				message.str("");//reset name
				message<<"1,3\n"<<alpha[a]<<","<<beta[b]<<","<<p_cap[p]<<"\n";
				stat_bucket.preamble = message.str();
				message.str("");//reset name
				message<<"output/sim"<<(a+b*alpha.size()+p*(alpha.size()+beta.size()))<<".csv";//set name
				stat_bucket.dump_to_file(message.str(),current_time);//write file
				//check the average delay:
				current_delay = stat_bucket.single_stat(packet_delay_avg,current_time);
				cout<<"average delay for alpha = "<<alpha[a]<<", beta = "<<beta[b]<<", p_cap = "<<p_cap[p]<<"is "<<current_delay<<"\n";
				if(current_delay<best_delay||best_delay<0)
				{
					best_delay=current_delay;
					best_alpha=alpha[a];
					best_beta=beta[b];
					best_p_cap=p_cap[p];
				}
			}
		}
	}
	
	cout<<"best average delay was "<<best_delay<<" for alpha = "<<best_alpha<<", beta = "<<best_beta<<", p_cap = "<<best_p_cap<<"\n";
}

void streamPass(ostream* s)
{
	(*s)<<"helloWorlds!\n";
}
/*-------------------------------------------------------------------*/
/*--------------------------- Experiments^ --------------------------*/
/*-------------------------------------------------------------------*/


int main(void)
{
	int unit_testing=0;
	if(unit_testing == 1)
	{
		streamPass(&cout);
		ofstream temp_output;
		temp_output.open("test_File!!.txt");
		streamPass(&temp_output);
		temp_output.close();
		//dc_flow_pattern_test();
		//markov_source_test();
		//qcsma_par_search();
		//test();//cout<<"\nend of test\n"<<iid_pkt_gen_test(.99,782)<<"\n";
		//cout<<iid_pkt_gen(.3)<<"\n";
		//		cout<<iid_pkt_gen(.7)<<"\n";
		//		cout<<iid_pkt_gen(.9)<<"\n";
		
		return 0;
	}

	sim_par.all_pkts_are_same=true;
	tcp_load_sim();//iid_load_sim();//
	
	//Print stats:
	cout<<"final time in clock_ticks = "<<current_time<<"\n";
	cout<<"final time in avg_pkt_lengths = "<<current_time/sched_par.avg_pkt_length<<"\n";
	cout<<"\n";
	
	//cout<<"pkts_generated:\n";
	int tot_pkts_generated=0;
	for(int f=0;f<num_flows;f++)
	{
		tot_pkts_generated+=pkts_generated[f];
		//cout<<pkts_delivered[s][d]<<" ";
	}
	cout<<"\n";
	cout<<"tot_pkts_generated = "<<tot_pkts_generated<<"\n";
	cout<<"\n";
	
	//cout<<"max_switch_Q:\n";
	int max_switch_Q=0;
	for(int s=0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			if(peak_switch_Q[s][d]>max_switch_Q)
			{
				max_switch_Q=peak_switch_Q[s][d];
			}
		}
		//cout<<"\n";
	}
	cout<<"\n";
	cout<<"max_switch_Q = "<<max_switch_Q<<"\n";
	cout<<"\n";
	
	//cout<<"pkts_delivered:\n";
	for(int s=0;s<row;s++)
	{
		for(int d=0;d<row;d++)
		{
			//cout<<pkts_delivered[s][d]<<" ";
		}
		//cout<<"\n";
	}
	cout<<"\n";
	cout<<"\n";
	
	//cout<<"avg_flow_rate:\n";
	int tot_pkts_delivered=0;
	for(int f=0;f<num_flows;f++)
	{
		tot_pkts_delivered+=flow_pkts_delivered[f];
		//cout<<(double)flow_pkts_delivered[f]/current_time<<" ";
	}
	cout<<"\n";
	cout<<"tot_pkts_delivered = "<<tot_pkts_delivered<<"\n";
	cout<<"avg_flow_rate = "<<(double) tot_pkts_delivered/current_time/row<<"\n";
	cout<<"frac switch capacity used = "<<(double) tot_pkts_delivered*sched_par.avg_pkt_length/current_time/row<<"\n";
	cout<<"frac switch capacity generated = "<<(double) tot_pkts_generated*sched_par.avg_pkt_length/current_time/row<<"\n";
	cout<<"\n";
	
	//cout<<"avg_flow_delay:\n";
	double tot_delay=0;
	for(int f=0;f<num_flows;f++)
	{
		if(flow_pkts_delivered[f]>0)
		{
			tot_delay+=tot_flow_delay[f];
			//cout<<(double)tot_flow_delay[f]/flow_pkts_delivered[f]/sched_par.avg_pkt_length<<" ";
		}
		else
		{
			//cout<<"-1 ";
		}
	}
	cout<<"\n";
	cout<<"tot_delay in clock ticks = "<<(double)tot_delay/tot_pkts_delivered<<"\n";
	cout<<"tot_delay in avg_pkt_lengths = "<<(double)tot_delay/tot_pkts_delivered/sched_par.avg_pkt_length<<"\n";
	cout<<"\n";
}
