%Flow_Grapher.m
%Plots delays experienced by different loads
clear all;
close all;
file_name = char('tcp_ideal_qcsma','tcp_slotted_qcsma','tcp_slip');%char('ideal_qcsma','slotted_qcsma','slip');%('flow');%
first_file = 1;%first file
last_file = 1;%last file
ideal_qcsma = zeros(1,last_file-first_file+1);
slotted_qcsma = zeros(1,last_file-first_file+1);
slip = zeros(1,last_file-first_file+1);
load = zeros(1,last_file-first_file+1);
frac_keep = .95;
max_range =0;
ht_on_2_off = .7;
for sim_type = 1:3
    for file = first_file:last_file
        input = csvread(strcat('./output/',file_name(sim_type,:),'_flow',int2str(file),'.csv'));%'logs/sim.csv');
        index = 1-first_file+file;
%         label = ['flow queue','switch queue','packet delays']
        %[w,h] = size(input);
        %imagesc(input.*(input<90).*(input>=0));colorbar;

        num_flows=input(1,1);
        row = input(1,2);
        avg_pkt_length=input(1,3);
        
        flow_src = input(2,1:num_flows);
        %figure;stem(flow_src);
        flow_dest = input(3,1:num_flows);
        %figure;stem(flow_dest);
        off_2_on = input(4,1:num_flows);
        %figure;stem(off_2_on);
        on_2_off = input(5,1:num_flows);
        %figure;stem(on_2_off);
        flow_gen_rate = input(6,1:num_flows);
        %figure;stem(flow_gen_rate);
        ht_percent=0;
        ds_percent=0;
        type_matrix = zeros(row,row);
        ht_filter = (on_2_off==ht_on_2_off);%has ones wherever there are ht flows
        for temp_f = 1:num_flows
            f=num_flows-temp_f+1;
            type = 2;
            ds_percent=ds_percent+1;
            if(on_2_off(f)==ht_on_2_off)
                type = 1;
                ht_percent=ht_percent+1;
                ds_percent=ds_percent-1;
            end
            type_matrix(flow_src(f)+1,flow_dest(f)+1)=type;
        end
        
        filter = [ht_filter;~ht_filter];%can isolate relevant flows
        if(sim_type==1)
            iq=figure;%ideal qcsma
        end
        if(sim_type==2)
            sq=figure;%slotted qcsma
        end
        if(sim_type==3)
            is=figure;%iterative slip
        end
        subplot(1,2,2);imagesc(type_matrix);colorbar;
        title(strcat(file_name(sim_type,:),'flows'))
        ds_percent = ds_percent/(row*row);
        ht_percent = ht_percent/(row*row);

        %Sim_Grapher.m
        %clear all;
        %close all;
        input = csvread(strcat('./output/',file_name(sim_type,:),'1.csv'));%'logs/sim.csv');
        %file_name = ['flow queue','switch queue','packet delays']
        [w,h] = size(input);
        %imagesc(input.*(input<90).*(input>=0));colorbar;
        
        
        dim_loc=[];%dimensions
        i=1;
        while i<w
            dim_loc = [dim_loc;i];
            i=i+input(i,1)+1;
        end
        %figure;
        rate_matrix=[];
        rate_mean=[];
        for flow_type_temp = 1:2%start with type 1;
            flow_type=flow_type_temp;
            %only do the first one first
            for i= 8:8%length(dim_loc)
                temp_index = dim_loc(i);
                data_matrix = input(temp_index+1:temp_index+input(temp_index,1),1:input(temp_index,2));
                if(sim_type==1)
                    delay_iq=data_matrix;
                end
                if(sim_type==2)
                    delay_sq = data_matrix;
                end
                if(sim_type==3)
                    delay_is=data_matrix;
                end
                q = data_matrix;
                q=q(q~=0);
                q=sort(q);
                mdd = q(ceil(length(q)*frac_keep));
                %figure;imagesc(data_matrix);colorbar;return;
                data=zeros(1,3);
                [a,b]=size(data_matrix);
                %filter=filter+(1-filter);
                %length(data_matrix)
                %sort(data_matrix);
                %data_matrix=data_matrix(1:floor(length(data_matrix)*frac_keep));
                if(i==8)
                    data_matrix = data_matrix(filter(flow_type,:));
                    data_matrix= data_matrix(data_matrix<mdd);
                    data_matrix = data_matrix(data_matrix~=0);
                end
                
                length(data_matrix)
                data(1) = min(min(data_matrix));
                data(3) = max(max(data_matrix));
                data(2) = sum(sum(data_matrix))/sum(sum(data_matrix~=0));
                data = data./avg_pkt_length;%rescale to allow the graphs to make sense.
                data = data*10;%because we have to adjust for round trip time
                if(max_range<data(3))
                    max_range=data(3);
                end
%                 set(errorbar([data(2)],[0],'r'),'linestyle','none','LineWidth',3)
%                 hold;
%                 set(errorbar([mean([data(1),data(3)])],[((data(3)-data(1))/2)],'b'),'linestyle','none','LineWidth',3)
%                 set(gca,'xtick',[1,2],'xticklabel',{'label appropriately','hello'})
%                 hold;
                
                rate_matrix = [rate_matrix; data(1) data(3)];
                rate_mean = [rate_mean; data(2)];
                
                %figure;
                %stem(data);%imagesc(data_matrix);colorbar;
                name='';
                switch(mod(i,3))
                    case 0
                        name = 'max';%title(strcat('max',label(i/1+1)));
                    case 1
                        name ='average';%title(strcat('average',label(i/1+1)));
                    case 2
                        name = 'variance';%title(strcat(label(i/1+1),'variance'));
                end
                switch(ceil(i/3))
                    case 1
                        name = strcat(name,' flow queue');%'max';%title(strcat('max',label(i/1+1)));
                    case 2
                        name =strcat(name,' switch queue');%title(strcat('average',label(i/1+1)));
                    case 3
                        name = strcat(name,' packet delay');%title(strcat(label(i/1+1),'variance'));
                end
                %title(name);
            end
        end

        subplot(1,2,1);%figure;
        rate_range = rate_matrix(:,2) - rate_matrix(:,1);
        %rate_range=rate_range/avg_pkt_length;%rescale.
        rate_diff = [rate_matrix(:,1) rate_range];
        %rate_diff=rate_diff/avg_pkt_length;%rescale
        
        eh = errorbar(mean(rate_matrix'), rate_range/2);
        set(eh,'linestyle','none','LineWidth',3);
        set(gca,'ylim',[0 max(rate_matrix(1,2),rate_matrix(2,2))*1.1]);
        set(gca,'xtick',[1 2],'xticklabel',{'type 1','type 2'});
        hold;
        eh = errorbar(rate_mean, [0;0],'r');
        set(eh,'linestyle','none','LineWidth',3);
        title(file_name(sim_type,:));
        ylabel('delay in average packet lengths');
        
    end
end
figure;subplot(2,1,1);delay_iq=sort(delay_iq(delay_iq~=0))/avg_pkt_length;stem(delay_iq);title('delay dist iq');
subplot(2,1,2);stem(delay_iq(1:24));title('restricted delay dist iq');
figure(iq);subplot(1,2,2);title('ideal qcsma');subplot(1,2,1);title('ideal qcsma'); set(gca,'ylim',[0 max_range*1.1]);

figure;subplot(2,1,1);delay_sq=sort(delay_sq(delay_sq~=0))/avg_pkt_length;stem(delay_sq);title('delay dist sq');
subplot(2,1,2);stem(delay_sq(1:24));title('restricted delay dist sq');

figure(sq);subplot(1,2,2);title('time slotted qcsma');subplot(1,2,1);title('time slotted qcsma');set(gca,'ylim',[0 max_range*1.1]);

figure;subplot(2,1,1);delay_is=sort(delay_is(delay_is~=0))/avg_pkt_length;stem(delay_is);title('delay dist is');
subplot(2,1,2);stem(delay_is(1:24));title('restricted delay dist is');

figure(is);subplot(1,2,2);title('iterative slip');subplot(1,2,1);title('iterative slip');set(gca,'ylim',[0 max_range*1.1]);

