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
ht_on_2_off = .7;
for sim_type = 1:3
    for file = first_file:last_file
        input = csvread(strcat('./output/',file_name(sim_type,:),'_flow',int2str(file),'.csv'));%'logs/sim.csv');
        index = 1-first_file+file;
%         label = ['flow queue','switch queue','packet delays']
        [w,h] = size(input);
        %imagesc(input.*(input<90).*(input>=0));colorbar;

        num_flows=input(1,1);
        row = input(1,2);
        
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
        for f = 1:num_flows
            type = 2;
            ds_percent=ds_percent+1;
            if(on_2_off(f)==ht_on_2_off)
                type = 1;
                ht_percent=ht_percent+1;
                ds_percent=ds_percent-1;
            end
            type_matrix(flow_src(f)+1,flow_dest(f)+1)=type;
        end
        
        figure;imagesc(type_matrix);colorbar;
        title(strcat(file_name(sim_type,:),'flows'))
        ds_percent = ds_percent/(row*row)
        ht_percent = ht_percent/(row*row)
    end
end